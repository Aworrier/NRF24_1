package com.example.nrfble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.example.nrfble.databinding.ActivityMainBinding
import java.nio.charset.StandardCharsets
import java.util.UUID

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val mainHandler = Handler(Looper.getMainLooper())

    private val serviceUuid: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private val cmdUuid: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private val notifyUuid: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private val cccdUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private var btAdapter: BluetoothAdapter? = null
    private var scanner: BluetoothLeScanner? = null
    private var targetDevice: BluetoothDevice? = null
    private var gatt: BluetoothGatt? = null
    private var cmdChar: BluetoothGattCharacteristic? = null
    private var notifyChar: BluetoothGattCharacteristic? = null

    private val requestPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            val ok = result.values.all { it }
            if (!ok) {
                toast("请先授予蓝牙权限")
            }
        }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            val name = result.scanRecord?.deviceName ?: device.name ?: "<unknown>"
            val filter = binding.etDeviceFilter.text.toString().trim()
            if (filter.isNotEmpty() && !name.contains(filter, ignoreCase = true)) {
                return
            }
            targetDevice = device
            binding.tvDevice.text = "已选设备: $name (${device.address})"
            appendLog("发现设备: $name ${device.address}")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            runOnUiThread {
                appendLog("连接状态变化: status=$status newState=$newState")
            }
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                gatt.discoverServices()
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                runOnUiThread {
                    appendLog("已断开 BLE")
                }
                cmdChar = null
                notifyChar = null
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread { appendLog("服务发现失败: $status") }
                return
            }

            val service: BluetoothGattService? = gatt.getService(serviceUuid)
            if (service == null) {
                runOnUiThread { appendLog("未找到 NUS 服务") }
                return
            }

            cmdChar = service.getCharacteristic(cmdUuid)
            notifyChar = service.getCharacteristic(notifyUuid)
            runOnUiThread {
                appendLog("服务发现成功，命令特征和通知特征已就绪")
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == notifyUuid) {
                val text = value.toString(StandardCharsets.UTF_8).trim()
                runOnUiThread { appendLog("设备通知: $text") }
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int
        ) {
            if (characteristic.uuid == notifyUuid && status == BluetoothGatt.GATT_SUCCESS) {
                val text = value.toString(StandardCharsets.UTF_8).trim()
                runOnUiThread { appendLog("设备读取: $text") }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val manager = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        btAdapter = manager.adapter
        scanner = btAdapter?.bluetoothLeScanner

        binding.btnScan.setOnClickListener { startScan() }
        binding.btnConnect.setOnClickListener { connectTarget() }
        binding.btnDisconnect.setOnClickListener { disconnectGatt() }
        binding.btnSubscribe.setOnClickListener { subscribeNotify() }

        binding.btnSend.setOnClickListener {
            val cmd = binding.etCommand.text.toString().trim()
            if (cmd.isNotEmpty()) {
                sendCommand(cmd)
            }
        }

        binding.btnEnable.setOnClickListener { sendCommand("ENABLE 1") }
        binding.btnDisable.setOnClickListener { sendCommand("ENABLE 0") }
        binding.btnStatus.setOnClickListener { sendCommand("STATUS") }

        checkAndRequestPermissions()
        appendLog("App ready. 按顺序操作：扫描 -> 连接 -> 订阅通知 -> 发送命令")
    }

    override fun onDestroy() {
        super.onDestroy()
        disconnectGatt()
    }

    private fun checkAndRequestPermissions() {
        val need = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (!hasPermission(Manifest.permission.BLUETOOTH_SCAN)) {
                need.add(Manifest.permission.BLUETOOTH_SCAN)
            }
            if (!hasPermission(Manifest.permission.BLUETOOTH_CONNECT)) {
                need.add(Manifest.permission.BLUETOOTH_CONNECT)
            }
        } else {
            if (!hasPermission(Manifest.permission.ACCESS_FINE_LOCATION)) {
                need.add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
        }

        if (need.isNotEmpty()) {
            requestPermissionLauncher.launch(need.toTypedArray())
        }
    }

    private fun hasPermission(permission: String): Boolean {
        return ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (btAdapter?.isEnabled != true) {
            toast("请先打开手机蓝牙")
            return
        }
        checkAndRequestPermissions()

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(serviceUuid))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        targetDevice = null
        binding.tvDevice.text = "扫描中..."
        appendLog("开始扫描 NUS 设备（10秒）")

        scanner?.startScan(listOf(filter), settings, scanCallback)
        mainHandler.postDelayed({
            scanner?.stopScan(scanCallback)
            appendLog("扫描结束")
        }, 10_000)
    }

    @SuppressLint("MissingPermission")
    private fun connectTarget() {
        val device = targetDevice
        if (device == null) {
            toast("请先扫描并选择设备")
            return
        }
        appendLog("开始连接: ${device.address}")
        gatt?.close()
        gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    private fun disconnectGatt() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        cmdChar = null
        notifyChar = null
    }

    @SuppressLint("MissingPermission")
    private fun subscribeNotify() {
        val g = gatt
        val n = notifyChar
        if (g == null || n == null) {
            toast("请先连接并发现服务")
            return
        }

        val ok = g.setCharacteristicNotification(n, true)
        if (!ok) {
            appendLog("setCharacteristicNotification 失败")
            return
        }

        val cccd = n.getDescriptor(cccdUuid)
        if (cccd == null) {
            appendLog("找不到 CCCD 描述符")
            return
        }

        val status = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
        }

        appendLog("订阅通知结果: $status")
    }

    @SuppressLint("MissingPermission")
    private fun sendCommand(command: String) {
        val g = gatt
        val c = cmdChar
        if (g == null || c == null) {
            toast("请先连接并发现服务")
            return
        }

        val payload = command.toByteArray(StandardCharsets.UTF_8)
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(c, payload, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                c.value = payload
                g.writeCharacteristic(c)
            }
        }

        if (ok) {
            appendLog("发送命令: $command")
        } else {
            appendLog("发送失败: $command")
        }
    }

    private fun appendLog(msg: String) {
        val now = System.currentTimeMillis() % 100000
        val line = "[$now] $msg\n"
        binding.tvLog.append(line)
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }
}
