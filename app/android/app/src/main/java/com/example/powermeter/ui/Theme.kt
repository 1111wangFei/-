package com.example.powermeter.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Colors = darkColorScheme(
    primary = Color(0xFF1E5EFF),
    onPrimary = Color.White,
    secondary = Color(0xFF4FC3F7),
    background = Color(0xFF0F1115),
    onBackground = Color(0xFFE8ECF3),
    surface = Color(0xFF171A21),
    onSurface = Color(0xFFE8ECF3)
)

@Composable
fun PowerMeterTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, content = content)
}
