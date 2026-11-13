package com.example.powermeter

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import com.example.powermeter.ui.MeterScreen
import com.example.powermeter.ui.PowerMeterTheme

class MainActivity : ComponentActivity() {

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { granted ->
        // 结果只是记录用；真正扫描时如果仍缺权限，BleManager 会给出提示
        if (granted.values.any { !it }) {
            // 用户拒绝也不阻塞界面，下次点扫描会再次申请
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            PowerMeterTheme {
                App()
            }
        }
    }

    @Composable
    private fun App() {
        val vm: MeterViewModel = viewModel()

        MeterScreen(
            vm = vm,
            onExport = { vm.buildShareIntent()?.let { share(it) } },
            onRequestPermissions = { requestBlePermissions() }
        )
    }

    /** Android 12 起用 BLUETOOTH_SCAN/CONNECT，之前用定位权限（BLE 扫描的系统要求） */
    private fun requestBlePermissions() {
        val needed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            listOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT
            )
        } else {
            listOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missing.isNotEmpty()) {
            permissionLauncher.launch(missing.toTypedArray())
        }
    }

    private fun share(intent: Intent) {
        startActivity(Intent.createChooser(intent, "导出历史数据"))
    }
}
