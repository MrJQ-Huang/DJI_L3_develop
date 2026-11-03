package dji.sampleV5.aircraft

import android.graphics.Color
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import dji.sdk.keyvalue.key.CameraKey
import dji.sdk.keyvalue.value.camera.CameraType
import dji.sdk.keyvalue.value.common.ComponentIndexType
import dji.v5.et.create
import dji.v5.et.get
import dji.v5.manager.KeyManager
import dji.v5.manager.SDKManager
import dji.v5.manager.datacenter.MediaDataCenter
import dji.v5.manager.interfaces.ICameraStreamManager
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

/** Camera-only reader. There are no flight, motor, pairing or gimbal commands. */
class L3ReadOnlyActivity : ComponentActivity(), SurfaceHolder.Callback {
    private data class Frame(val bytes: ByteArray, val width: Int, val height: Int,
                             val sequence: Long, val callbackTimeNs: Long)

    private val main = Handler(Looper.getMainLooper())
    private val pending = ArrayBlockingQueue<Frame>(1)
    private val received = AtomicLong()
    private val sampled = AtomicLong()
    private val sent = AtomicLong()
    private val lastSampleNs = AtomicLong()
    private val streamManager get() = MediaDataCenter.getInstance().cameraStreamManager
    private lateinit var status: TextView
    private lateinit var preview: SurfaceView
    private var cameras: List<ComponentIndexType> = emptyList()
    private val cameraTypes = mutableMapOf<ComponentIndexType, CameraType>()
    private var currentSource = "等待硬件读取"
    private var sourceRange = "等待硬件读取"
    private var metadataReadAt = 0L
    private var selected: ComponentIndexType? = null
    private var listening = false
    @Volatile private var alive = true
    @Volatile private var foreground = false
    @Volatile private var sending = true
    @Volatile private var socket: Socket? = null
    @Volatile private var writeStartedAt = 0L
    @Volatile private var networkStatus = "等待电脑接收程序"
    @Volatile private var imageSize = "等待图像"
    private lateinit var sender: Thread

    private val availableListener = object : ICameraStreamManager.AvailableCameraUpdatedListener {
        override fun onAvailableCameraUpdated(list: MutableList<ComponentIndexType>) {
            main.post {
                if (!alive) return@post
                cameras = list.toList()
                cameraTypes.keys.retainAll(cameras.toSet())
                readMetadata()
                findL3()
            }
        }
        override fun onCameraStreamEnableUpdate(map: MutableMap<ComponentIndexType, Boolean>) = Unit
    }

    private val frameListener = object : ICameraStreamManager.CameraFrameListener {
        override fun onFrame(data: ByteArray, offset: Int, length: Int, width: Int, height: Int,
                             format: ICameraStreamManager.FrameFormat) {
            received.incrementAndGet()
            if (!alive || !foreground || !sending || format != ICameraStreamManager.FrameFormat.NV21) return
            val now = SystemClock.elapsedRealtimeNanos()
            val previous = lastSampleNs.get()
            if (now - previous < 200_000_000L || !lastSampleNs.compareAndSet(previous, now)) return
            val required = width.toLong() * height * 3 / 2
            if (width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0 ||
                required > 32_000_000L || required > length || offset < 0 ||
                offset.toLong() + required > data.size) return
            // The SDK owns its buffer. Copy before returning and keep at most one waiting frame.
            val frame = Frame(data.copyOfRange(offset, offset + required.toInt()), width, height,
                sampled.incrementAndGet(), now)
            if (!pending.offer(frame)) { pending.poll(); pending.offer(frame) }
        }
    }

    private val refresh = object : Runnable {
        override fun run() {
            if (!alive) return
            try {
                if (writeStartedAt != 0L && SystemClock.elapsedRealtime() - writeStartedAt > 3000) {
                    socket?.close()
                    pending.clear()
                }
                if (!listening && SDKManager.getInstance().isRegistered) {
                    streamManager.addAvailableCameraUpdatedListener(availableListener)
                    listening = true
                }
                if (listening) { readMetadata(); findL3() }
                val index = selected
                status.text = "L3 相机取数 | SDK 注册：${SDKManager.getInstance().isRegistered}\n" +
                    "相机：${index?.name ?: "等待 ZENMUSE_L3"} | 视频源：$currentSource | 可用源：$sourceRange\n" +
                    "$imageSize | SDK帧：${received.get()} | 已发送：${sent.get()}（最多5帧/秒）\n" +
                    "$networkStatus | 仅订阅 L3 视频，不发送飞控命令"
            } catch (error: Exception) {
                status.text = "等待 SDK / L3 就绪：${error.javaClass.simpleName}"
            }
            main.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
        }
        status = TextView(this).apply {
            setTextColor(Color.WHITE); textSize = 16f; setPadding(16, 8, 16, 8)
            text = "等待 L3 相机，地面取数模式"
        }
        layout.addView(status)
        val pause = Button(this).apply {
            text = "暂停向电脑传输图像"
            setOnClickListener {
                sending = !sending
                if (!sending) { pending.clear(); socket?.close() }
                text = if (sending) "暂停向电脑传输图像" else "恢复向电脑传输图像"
            }
        }
        layout.addView(pause, LinearLayout.LayoutParams(-1, -2))
        preview = SurfaceView(this)
        preview.holder.addCallback(this)
        layout.addView(preview, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(layout)
        sender = Thread({ sendFrames() }, "L3-frame-sender").apply { start() }
        main.post(refresh)
    }

    private fun readMetadata() {
        val now = SystemClock.elapsedRealtime()
        if (now - metadataReadAt < 2000) return
        metadataReadAt = now
        for (index in cameras) {
            // The callback overload reads hardware; the synchronous getValue only reads cache.
            CameraKey.KeyCameraType.create(index).get({ type ->
                main.post {
                    if (alive && index in cameras && type != null) {
                        cameraTypes[index] = type
                        findL3()
                    }
                }
            }, { error -> Log.d("L3ReadOnly", "Camera metadata $index: ${error.errorCode()}") })
        }
        selected?.let { index ->
            CameraKey.KeyCameraVideoStreamSource.create(index).get({ value ->
                main.post { if (selected == index) currentSource = value?.name ?: "UNKNOWN" }
            }, { })
            CameraKey.KeyCameraVideoStreamSourceRange.create(index).get({ value ->
                main.post { if (selected == index) sourceRange = value.toString() }
            }, { })
        }
    }

    private fun findL3() {
        val index = cameras.firstOrNull {
            (cameraTypes[it] ?: KeyManager.getInstance().getValue(CameraKey.KeyCameraType.create(it))) == CameraType.ZENMUSE_L3
        }
        if (index == selected) return
        if (selected != null) {
            streamManager.removeFrameListener(frameListener)
            streamManager.removeCameraStreamSurface(preview.holder.surface)
            pending.clear()
            socket?.close()
        }
        selected = null
        if (index != null) {
            Log.i("L3ReadOnly", "Selected camera=$index type=ZENMUSE_L3")
            streamManager.addFrameListener(index, ICameraStreamManager.FrameFormat.NV21, frameListener)
            selected = index
            attachPreview()
        }
    }

    private fun attachPreview() {
        val index = selected ?: return
        if (!foreground || !preview.holder.surface.isValid || preview.width <= 0 || preview.height <= 0) return
        streamManager.putCameraStreamSurface(index, preview.holder.surface, preview.width, preview.height,
            ICameraStreamManager.ScaleType.CENTER_INSIDE)
    }

    private fun sendFrames() {
        var output: DataOutputStream? = null
        while (alive) {
            try {
                val frame = pending.poll(500, TimeUnit.MILLISECONDS) ?: continue
                if (!sending || !foreground) continue
                val jpeg = ByteArrayOutputStream().use {
                    check(YuvImage(frame.bytes, ImageFormat.NV21, frame.width, frame.height, null)
                        .compressToJpeg(Rect(0, 0, frame.width, frame.height), 80, it))
                    it.toByteArray()
                }
                if (!alive || !sending || !foreground) continue
                if (output == null) {
                    val connected = Socket()
                    socket = connected
                    connected.tcpNoDelay = true
                    connected.sendBufferSize = 128 * 1024
                    connected.connect(InetSocketAddress("127.0.0.1", 9000), 1500)
                    output = DataOutputStream(connected.getOutputStream())
                }
                if (!alive || !sending || !foreground) {
                    socket?.close()
                    output = null
                    continue
                }
                // Big endian: L3F1, JPEG length, width, height, sequence, callback monotonic ns.
                // callbackTimeNs is NOT sensor exposure time or a synchronized timestamp.
                writeStartedAt = SystemClock.elapsedRealtime()
                output.writeInt(0x4c334631)
                output.writeInt(jpeg.size)
                output.writeInt(frame.width)
                output.writeInt(frame.height)
                output.writeLong(frame.sequence)
                output.writeLong(frame.callbackTimeNs)
                output.write(jpeg)
                output.flush()
                writeStartedAt = 0L
                sent.incrementAndGet()
                imageSize = "${frame.width}×${frame.height} / NV21 → JPEG → 电脑BGR"
                networkStatus = "USB 图像传输已连接"
            } catch (_: InterruptedException) {
                break
            } catch (error: Exception) {
                writeStartedAt = 0L
                networkStatus = "等待电脑：${error.javaClass.simpleName}"
                try { socket?.close() } catch (_: Exception) { }
                socket = null
                output = null
                pending.clear()
                if (alive) try { Thread.sleep(700) } catch (_: InterruptedException) { break }
            }
        }
        try { socket?.close() } catch (_: Exception) { }
    }

    override fun onResume() {
        super.onResume()
        foreground = true
        if (::preview.isInitialized) attachPreview()
    }

    override fun onPause() {
        foreground = false
        pending.clear()
        socket?.close()
        if (selected != null) streamManager.removeCameraStreamSurface(preview.holder.surface)
        super.onPause()
    }

    override fun surfaceCreated(holder: SurfaceHolder) = attachPreview()
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = attachPreview()
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (selected != null) streamManager.removeCameraStreamSurface(holder.surface)
    }

    override fun onDestroy() {
        alive = false
        foreground = false
        main.removeCallbacksAndMessages(null)
        pending.clear()
        socket?.close()
        sender.interrupt()
        if (listening) {
            streamManager.removeAvailableCameraUpdatedListener(availableListener)
            streamManager.removeFrameListener(frameListener)
            streamManager.removeCameraStreamSurface(preview.holder.surface)
        }
        super.onDestroy()
    }
}
