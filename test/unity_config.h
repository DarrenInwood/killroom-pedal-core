// Unity configuration for host-native and embedded builds.
//
// PlatformIO looks for this file when configuring the Unity test framework.
// The native env (pio test -e native) uses stdio by default and ignores these
// overrides.  The firmware envs (multi_effect_dev / multi_effect) need at least a
// minimal UNITY_OUTPUT_CHAR so the Unity source compiles against CMSIS.
//
// Note: embedded tests still require physical hardware to upload and run.
// The primary test suite runs natively:  pio test -e native

#ifndef UNITY_CONFIG_H
#define UNITY_CONFIG_H

#ifdef STM32F411xE
// Bare-metal STM32 build: route Unity output to USART1 (MIDI TX, PA9).
// USART1 must be initialised before any Unity calls; call unity_setup() first
// if writing an embedded test harness (MIDI baud is 31250).
#include <stm32f4xx.h>

static inline void unity_putc(char c) {
    while (!(USART1->SR & USART_SR_TXE));
    USART1->DR = (uint8_t)c;
}

#define UNITY_OUTPUT_CHAR(a)  unity_putc(a)
#define UNITY_OUTPUT_FLUSH()
#endif  // STM32F411xE

#endif  // UNITY_CONFIG_H
