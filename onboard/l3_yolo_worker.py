#!/usr/bin/python3
"""YOLOX-s on JetPack 5: compress annotated L3 frames for the existing video relay."""

import argparse
import ctypes
import os
from pathlib import Path
from queue import Empty, Full, Queue
import sys
import threading
import time

import cv2
import numpy as np

SIZE = 640
INFERENCE_PERIOD = 0.1
MAX_BOX_AGE = 0.25


def prediction_grid():
    grids, strides = [], []
    for stride in (8, 16, 32):
        y, x = np.mgrid[:SIZE // stride, :SIZE // stride]
        grids.append(np.stack((x, y), axis=-1).reshape(-1, 2))
        strides.append(np.full((x.size, 1), stride, dtype=np.float32))
    return np.concatenate(grids).astype(np.float32), np.concatenate(strides)


GRID, STRIDES = prediction_grid()


def prepare_image(image):
    height, width = image.shape[:2]
    scale = min(SIZE / width, SIZE / height)
    resized = cv2.resize(image, (int(width * scale), int(height * scale)))
    padded = np.full((SIZE, SIZE, 3), 114, dtype=np.uint8)
    padded[:resized.shape[0], :resized.shape[1]] = resized[:, :, :3]
    return np.ascontiguousarray(padded.transpose(2, 0, 1)[None], dtype=np.float32), scale


def detect_people(output, width, height, scale):
    predictions = output.reshape(8400, 85).astype(np.float32, copy=False)
    scores = predictions[:, 4] * predictions[:, 5]
    keep = np.isfinite(scores) & (scores >= 0.35)
    centers = (predictions[keep, :2] + GRID[keep]) * STRIDES[keep]
    sizes = np.exp(np.clip(predictions[keep, 2:4], -16, 16)) * STRIDES[keep]
    corners = (centers - sizes / 2) / scale
    ends = (centers + sizes / 2) / scale
    corners = np.clip(corners, (0, 0), (width - 1, height - 1))
    ends = np.clip(ends, (0, 0), (width - 1, height - 1))
    valid = np.isfinite(corners).all(axis=1) & ((ends - corners) >= 1).all(axis=1)
    boxes = np.concatenate((corners[valid], (ends - corners)[valid]), axis=1).astype(int)
    confidence = scores[keep][valid]
    if not len(boxes):
        return []
    selected = cv2.dnn.NMSBoxes(boxes.tolist(), confidence.tolist(), 0.349, 0.45)
    return [(boxes[i], confidence[i]) for i in np.asarray(selected).reshape(-1)]


class TensorRT:
    def __init__(self, model):
        import tensorrt as trt

        self.cuda = ctypes.CDLL("libcudart.so.11.0")
        self.cuda.cudaMalloc.argtypes = (ctypes.POINTER(ctypes.c_void_p), ctypes.c_size_t)
        self.cuda.cudaFree.argtypes = (ctypes.c_void_p,)
        self.cuda.cudaMemcpy.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)
        self.logger = trt.Logger(trt.Logger.WARNING)
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(Path(model).read_bytes())
        if self.engine is None or self.engine.num_bindings != 2 or self.engine.has_implicit_batch_dimension:
            raise RuntimeError("expected an explicit-batch YOLOX-s TensorRT engine")
        self.input_index = next(i for i in range(2) if self.engine.binding_is_input(i))
        self.output_index = 1 - self.input_index
        self.buffers = [np.empty(tuple(self.engine.get_binding_shape(i)),
                                 dtype=trt.nptype(self.engine.get_binding_dtype(i))) for i in range(2)]
        if self.buffers[self.input_index].shape != (1, 3, SIZE, SIZE) or self.buffers[self.output_index].shape != (1, 8400, 85):
            raise RuntimeError("unexpected YOLOX-s engine dimensions")
        if any(buf.dtype not in (np.float32, np.float16) for buf in self.buffers):
            raise RuntimeError("expected floating point YOLOX-s bindings")
        self.context = self.engine.create_execution_context()
        if self.context is None:
            raise RuntimeError("could not initialize TensorRT execution context")
        self.device = [ctypes.c_void_p(), ctypes.c_void_p()]
        try:
            for pointer, buf in zip(self.device, self.buffers):
                self.check_cuda(self.cuda.cudaMalloc(ctypes.byref(pointer), buf.nbytes))
        except Exception:
            self.close()
            raise

    @staticmethod
    def check_cuda(code):
        if code:
            raise RuntimeError(f"CUDA error {code}")

    def infer(self, image):
        source = self.buffers[self.input_index]
        np.copyto(source, image, casting="same_kind")
        self.check_cuda(self.cuda.cudaMemcpy(self.device[self.input_index], source.ctypes.data, source.nbytes, 1))
        if not self.context.execute_v2([pointer.value for pointer in self.device]):
            raise RuntimeError("TensorRT execution failed")
        output = self.buffers[self.output_index]
        self.check_cuda(self.cuda.cudaMemcpy(output.ctypes.data, self.device[self.output_index], output.nbytes, 2))
        return output

    def close(self):
        self.context = None
        for pointer in self.device:
            if pointer.value:
                self.cuda.cudaFree(pointer)
        self.engine = None
        self.runtime = None


def offer(queue, item):
    try:
        queue.put_nowait(item)
    except Full:
        pass


def replace_latest(queue, item):
    try:
        queue.get_nowait()
    except Empty:
        pass
    queue.put_nowait(item)


def infer_frames(model, frames, results, errors, ready, stopping):
    network = None
    try:
        network = TensorRT(model)
        network.infer(np.zeros((1, 3, SIZE, SIZE), dtype=np.float32))
        ready.set()
        while not stopping.is_set():
            try:
                timestamp, image = frames.get(timeout=0.1)
            except Empty:
                continue
            values, scale = prepare_image(image)
            height, width = image.shape[:2]
            boxes = detect_people(network.infer(values), width, height, scale)
            replace_latest(results, (timestamp, boxes))
    except Exception as error:
        offer(errors, error)
    finally:
        if network is not None:
            network.close()


def feed_h264(gst, source, errors, stopping):
    try:
        while not stopping.is_set():
            data = os.read(0, 65536)
            if not data:
                stopping.set()
                return
            packet = gst.Buffer.new_wrapped(data)
            if source.emit("push-buffer", packet) != gst.FlowReturn.OK:
                raise RuntimeError("H.264 input stopped accepting buffers")
    except Exception as error:
        offer(errors, error)


def check_pipeline(gst, pipeline):
    message = pipeline.get_bus().pop_filtered(gst.MessageType.ERROR | gst.MessageType.EOS)
    if message is not None:
        if message.type == gst.MessageType.ERROR:
            error, details = message.parse_error()
            raise RuntimeError(f"{error.message} ({details})")
        raise RuntimeError("unexpected end of GStreamer pipeline")


def copy_frame(gst, video, sample):
    info = video.VideoInfo.new_from_caps(sample.get_caps())
    if info is None:
        raise RuntimeError("invalid raw frame caps")
    packet = sample.get_buffer()
    meta = video.buffer_get_video_meta(packet)
    stride, offset = (meta.stride[0], meta.offset[0]) if meta else (info.stride[0], info.offset[0])
    ok, mapped = packet.map(gst.MapFlags.READ)
    if not ok:
        raise RuntimeError("could not map decoded frame")
    try:
        frame = np.ndarray((info.height, info.width, 4), dtype=np.uint8, buffer=mapped.data,
                           strides=(stride, 4, 1), offset=offset).copy()
    finally:
        packet.unmap(mapped)
    return frame, (info.fps_n or 30, info.fps_d or 1), packet.pts


def make_encoder(gst, width, height, fps, bitrate, media_fd):
    key_interval = max(1, round(fps[0] / fps[1]))
    return gst.parse_launch(
        'appsrc name=images is-live=true block=true format=time max-bytes=16588800 '
        f'caps="video/x-raw,format=BGRx,width={width},height={height},framerate={fps[0]}/{fps[1]}" '
        '! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 '
        f'! nvv4l2h264enc bitrate={bitrate} insert-sps-pps=true idrinterval={key_interval} '
        f'iframeinterval={key_interval} '
        '! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au '
        f'! fdsink fd={media_fd} sync=false async=false'
    )


def draw_boxes(image, boxes):
    for (x, y, width, height), score in boxes:
        color = (60, 255, 80, 255)
        cv2.rectangle(image, (x, y), (x + width, y + height), color, 3)
        cv2.putText(image, f"person {score:.0%}", (x, max(y - 7, 21)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv2.LINE_AA)


def video_loop(gst, video, decoder, bitrate, media_fd, frames, results, errors, stopping):
    output = None
    encoder_input = None
    first_pts = None
    index = 0
    last_pts = -1
    last_sample = time.monotonic()
    next_inference = 0
    last_detection, boxes = 0, []
    sink = decoder.get_by_name("decoded")
    try:
        while not stopping.is_set():
            if not errors.empty():
                raise errors.get_nowait()
            check_pipeline(gst, decoder)
            if output is not None:
                check_pipeline(gst, output)
            sample = sink.emit("try-pull-sample", 100 * gst.MSECOND)
            now = time.monotonic()
            if sample is None:
                if now - last_sample > 8:
                    raise RuntimeError("no decoded L3 frame for eight seconds")
                continue
            last_sample = now
            frame, fps, pts = copy_frame(gst, video, sample)
            if frame.shape[:2] != (1080, 1920):
                raise RuntimeError(f"expected 1920x1080 input, received {frame.shape[1]}x{frame.shape[0]}")
            if now >= next_inference:
                replace_latest(frames, (now, frame.copy()))
                next_inference = now + INFERENCE_PERIOD
            try:
                last_detection, boxes = results.get(timeout=1) if index == 0 else results.get_nowait()
            except Empty:
                if index == 0:
                    continue
            if output is None:
                output = make_encoder(gst, frame.shape[1], frame.shape[0], fps, bitrate, media_fd)
                encoder_input = output.get_by_name("images")
                if output.set_state(gst.State.PLAYING) == gst.StateChangeReturn.FAILURE:
                    raise RuntimeError("could not start hardware H.264 encoder")
                print(f"event=yolo_worker phase=first_inference persons={len(boxes)}", file=sys.stderr, flush=True)
            if time.monotonic() - last_detection <= MAX_BOX_AGE:
                draw_boxes(frame, boxes)
            duration = gst.SECOND * fps[1] // fps[0]
            if pts != gst.CLOCK_TIME_NONE and first_pts is None:
                first_pts = pts - index * duration
            timeline = index * duration if pts == gst.CLOCK_TIME_NONE else pts - first_pts
            packet = gst.Buffer.new_wrapped(frame.tobytes())
            packet.pts = max(timeline, last_pts + duration, 0)
            packet.duration = duration
            last_pts = packet.pts
            index += 1
            if encoder_input.emit("push-buffer", packet) != gst.FlowReturn.OK:
                raise RuntimeError("H.264 encoder stopped accepting frames")
    finally:
        if output is not None:
            output.set_state(gst.State.NULL)


def run(model, bitrate, media_fd):
    import gi

    gi.require_version("Gst", "1.0")
    gi.require_version("GstVideo", "1.0")
    from gi.repository import Gst, GstVideo

    Gst.init(None)
    for plugin in ("h264parse", "nvv4l2decoder", "nvvidconv", "nvv4l2h264enc", "appsrc", "appsink", "fdsink"):
        if Gst.ElementFactory.find(plugin) is None:
            raise RuntimeError(f"missing GStreamer element {plugin}")
    frames, results, errors = Queue(1), Queue(1), Queue(1)
    ready, stopping = threading.Event(), threading.Event()
    inference = threading.Thread(target=infer_frames, args=(model, frames, results, errors, ready, stopping), daemon=True)
    inference.start()
    decoder = None
    try:
        while not ready.wait(0.1):
            if not errors.empty():
                raise errors.get_nowait()
        decoder = Gst.parse_launch(
            'appsrc name=compressed is-live=true block=true max-bytes=1048576 '
            'caps="video/x-h264,stream-format=byte-stream" '
            '! h264parse ! nvv4l2decoder ! nvvidconv ! video/x-raw,format=BGRx '
            '! appsink name=decoded sync=false max-buffers=2 drop=true'
        )
        if decoder.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("could not start hardware H.264 decoder")
        os.write(media_fd, b"R")
        threading.Thread(target=feed_h264, args=(Gst, decoder.get_by_name("compressed"), errors, stopping), daemon=True).start()
        video_loop(Gst, GstVideo, decoder, bitrate, media_fd, frames, results, errors, stopping)
    finally:
        stopping.set()
        if decoder is not None:
            decoder.set_state(Gst.State.NULL)
        inference.join(timeout=0.5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--bitrate", type=int, required=True)
    args = parser.parse_args()
    # Some native NVIDIA plugins write diagnostic text to stdout. Give the
    # media sink its own descriptor so those prints never enter Annex B.
    media_fd = os.dup(1)
    os.dup2(2, 1)
    try:
        run(args.engine, args.bitrate, media_fd)
    except Exception as exc:
        print(f"event=yolo_worker phase=failed error={exc}", file=sys.stderr, flush=True)
        sys.exit(1)
    finally:
        os.close(media_fd)
