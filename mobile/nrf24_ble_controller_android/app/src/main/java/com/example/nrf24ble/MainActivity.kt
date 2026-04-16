package com.example.nrf24ble

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.EditText
import android.widget.RadioButton
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

class MainActivity : AppCompatActivity() {

    private val serviceUuid: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private val cmdUuid: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private val notifyUuid: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private val cccdUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")

    private val mainHandler = Handler(Looper.getMainLooper())
    private val tsFmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())

    private lateinit var tvConnState: TextView
    private lateinit var tvLog: TextView

    private lateinit var etCount: EditText
    private lateinit var etInterval: EditText
    private lateinit var etPayload: EditText
    private lateinit var rbAscii: RadioButton

    private var bluetoothAdapter: BluetoothAdapter? = null
    private var gatt: BluetoothGatt? = null
    private var cmdCharacteristic: BluetoothGattCharacteristic? = null
    private var notifyCharacteristic: BluetoothGattCharacteristic? = null
    private var scanInProgress = false

    private val requiredPermissions: Array<String>
        get() {
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                arrayOf(
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT
                )
            } else {
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
            }
        }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        val granted = result.values.all { it }
        if (granted) {
            scanAndConnect()
        } else {
            toast("权限未授权，无法进行 BLE 连接")
        }
    }

    private val stopScanRunnable = Runnable {
        stopScan()
        if (gatt == null) {
            appendLog("扫描结束，未发现目标设备")
            setConnState("状态: 未连接")
        }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val device = result?.device ?: return
            appendLog("发现设备: ${device.address} ${device.name ?: "(no-name)"}")
            stopScan()
            connectDevice(result)
        }

        override fun onScanFailed(errorCode: Int) {
            stopScan()
            appendLog("扫描失败: error=$errorCode")
            setConnState("状态: 扫描失败")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            mainHandler.post {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    appendLog("连接状态异常: status=$status")
                    closeGatt()
                    setConnState("状态: 连接失败")
                    return@post
                }

                when (newState) {
                    android.bluetooth.BluetoothProfile.STATE_CONNECTED -> {
                        appendLog("BLE 已连接: ${gatt.device.address}")
                        setConnState("状态: 已连接，发现服务中")
                        if (!checkConnectPermission()) {
                            appendLog("缺少连接权限，无法发现服务")
                            return@post
                        }
                        gatt.discoverServices()
                    }

                    android.bluetooth.BluetoothProfile.STATE_DISCONNECTED -> {
                        appendLog("BLE 已断开")
                        closeGatt()
                        setConnState("状态: 未连接")
                    }
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            mainHandler.post {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    appendLog("服务发现失败: status=$status")
                    return@post
                }
                val service = gatt.getService(serviceUuid)
                if (service == null) {
                    appendLog("未找到目标服务: $serviceUuid")
                    return@post
                }

                cmdCharacteristic = service.getCharacteristic(cmdUuid)
                notifyCharacteristic = service.getCharacteristic(notifyUuid)
                if (cmdCharacteristic == null || notifyCharacteristic == null) {
                    appendLog("目标特征缺失，cmd=${cmdCharacteristic != null}, notify=${notifyCharacteristic != null}")
                    return@post
                }

                enableNotifications(gatt, notifyCharacteristic!!)
                setConnState("状态: 已连接，可发送命令")
                appendLog("服务就绪，可执行 STATUS / BURST 等命令")
                sendCommand("STATUS")
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid != notifyUuid) {
                return
            }
            val msg = value.toString(Charsets.UTF_8).trim()
            mainHandler.post {
                appendLog("<= $msg")
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (characteristic.uuid != cmdUuid) {
                return
            }
            mainHandler.post {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    appendLog("写入成功")
                } else {
                    appendLog("写入失败: status=$status")
                }
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            mainHandler.post {
                if (descriptor.uuid == cccdUuid) {
                    appendLog("通知使能结果: status=$status")
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        val manager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = manager.adapter

        tvConnState = findViewById(R.id.tvConnState)
        tvLog = findViewById(R.id.tvLog)

        etCount = findViewById(R.id.etCount)
        etInterval = findViewById(R.id.etInterval)
        etPayload = findViewById(R.id.etPayload)
        rbAscii = findViewById(R.id.rbAscii)

        findViewById<Button>(R.id.btnScan).setOnClickListener {
            ensurePermissionsAndScan()
        }
        findViewById<Button>(R.id.btnDisconnect).setOnClickListener {
            disconnectGatt()
        }
        findViewById<Button>(R.id.btnEnable).setOnClickListener {
            sendCommand("ENABLE 1")
        }
        findViewById<Button>(R.id.btnDisable).setOnClickListener {
            sendCommand("ENABLE 0")
        }
        findViewById<Button>(R.id.btnStatus).setOnClickListener {
            sendCommand("STATUS")
        }
        findViewById<Button>(R.id.btnReset).setOnClickListener {
            sendCommand("RESETSTATS")
        }
        findViewById<Button>(R.id.btnStop).setOnClickListener {
            sendCommand("STOP")
        }
        findViewById<Button>(R.id.btnBurst).setOnClickListener {
            sendBurstCommand()
        }

        appendLog("应用启动，点击“扫描并连接”开始")
    }

    override fun onDestroy() {
        stopScan()
        disconnectGatt()
        super.onDestroy()
    }

    private fun ensurePermissionsAndScan() {
        if (!hasAllPermissions()) {
            permissionLauncher.launch(requiredPermissions)
            return
        }
        scanAndConnect()
    }

    private fun hasAllPermissions(): Boolean {
        return requiredPermissions.all {
            ContextCompat.checkSelfPermission(this, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun checkConnectPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return true
        }
        return ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    }

    private fun scanAndConnect() {
        val adapter = bluetoothAdapter
        if (adapter == null || !adapter.isEnabled) {
            toast("请先打开手机蓝牙")
            return
        }

        disconnectGatt()

        val scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            toast("当前设备不支持 BLE 扫描")
            return
        }

        val filter = ScanFilter.Builder()
            .setServiceUuid(android.os.ParcelUuid(serviceUuid))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        appendLog("开始扫描目标设备...")
        setConnState("状态: 扫描中")
        scanInProgress = true
        scanner.startScan(listOf(filter), settings, scanCallback)
        mainHandler.postDelayed(stopScanRunnable, 12_000)
    }

    private fun stopScan() {
        if (!scanInProgress) {
            return
        }
        val adapter = bluetoothAdapter ?: return
        val scanner = adapter.bluetoothLeScanner ?: return
        try {
            scanner.stopScan(scanCallback)
        } catch (_: SecurityException) {
            appendLog("停止扫描时权限不足")
        }
        scanInProgress = false
        mainHandler.removeCallbacks(stopScanRunnable)
    }

    private fun connectDevice(result: ScanResult) {
        val device = result.device ?: run {
            appendLog("扫描结果无设备对象")
            return
        }
        if (!checkConnectPermission()) {
            toast("缺少蓝牙连接权限")
            return
        }

        appendLog("连接设备: ${device.address}")
        setConnState("状态: 连接中")
        gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(this, false, gattCallback, BluetoothGatt.TRANSPORT_LE)
        } else {
            device.connectGatt(this, false, gattCallback)
        }
    }

    private fun disconnectGatt() {
        gatt?.let {
            try {
                it.disconnect()
            } catch (_: SecurityException) {
            }
        }
        closeGatt()
        setConnState("状态: 未连接")
    }

    private fun closeGatt() {
        gatt?.close()
        gatt = null
        cmdCharacteristic = null
        notifyCharacteristic = null
    }

    private fun enableNotifications(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        if (!checkConnectPermission()) {
            return
        }

        gatt.setCharacteristicNotification(characteristic, true)
        val cccd = characteristic.getDescriptor(cccdUuid)
        if (cccd == null) {
            appendLog("通知描述符不存在")
            return
        }

        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        gatt.writeDescriptor(cccd)
    }

    private fun sendBurstCommand() {
        val count = etCount.text.toString().trim().toIntOrNull()
        val interval = etInterval.text.toString().trim().toIntOrNull()
        val payload = etPayload.text.toString().trim()

        if (count == null || count <= 0) {
            toast("count 必须为正整数")
            return
        }
        if (interval == null || interval < 0) {
            toast("interval 必须为非负整数")
            return
        }
        if (payload.isEmpty()) {
            toast("payload 不能为空")
            return
        }

        val cmd = if (rbAscii.isChecked) {
            "BURST $count $interval $payload"
        } else {
            if (payload.length % 2 != 0 || !payload.matches(Regex("^[0-9a-fA-F]+$"))) {
                toast("HEX 模式 payload 必须是偶数长度十六进制")
                return
            }
            "BURSTHEX $count $interval $payload"
        }
        sendCommand(cmd)
    }

    private fun sendCommand(command: String) {
        val gattRef = gatt
        val cmdChr = cmdCharacteristic
        if (gattRef == null || cmdChr == null) {
            toast("设备未连接或服务未就绪")
            return
        }
        if (!checkConnectPermission()) {
            toast("缺少蓝牙连接权限")
            return
        }

        cmdChr.value = (command + "\n").toByteArray(Charsets.UTF_8)
        cmdChr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

        appendLog("=> $command")
        val ok = gattRef.writeCharacteristic(cmdChr)
        if (!ok) {
            appendLog("命令发送失败")
        }
    }

    private fun setConnState(text: String) {
        tvConnState.text = text
    }

    private fun appendLog(msg: String) {
        val prev = tvLog.text.toString()
        val line = "${tsFmt.format(Date())}  $msg"
        val merged = if (prev.isEmpty()) line else "$prev\n$line"
        tvLog.text = merged
    }

    private fun toast(text: String) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show()
    }
}
