/*
 * Copyright (C) 2026 The ZeroG Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// Web Audio's FFT for ZeroG (patch 0030), over pffft's real transform. The
// frequency data follows the Accelerate convention the shared FFTFrame code
// expects (FFTFrameCocoa.cpp): N/2 values, the mathematical (unnormalized) DFT,
// with the DC component in realData()[0] and the Nyquist one packed in
// imagData()[0]; the inverse divides by N, so IFFT(FFT(x)) == x. pffft takes
// sizes that are multiples of 32; smaller ones use a direct DFT.

#include "config.h"
#include "FFTFrame.h"

#if ENABLE(WEB_AUDIO) && PLATFORM(ZEROG)

#include "VectorMath.h"
#include <cmath>
#include <numbers>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <pffft/pffft.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

namespace {

constexpr unsigned minimumPow2Size = 2;
constexpr unsigned maximumPow2Size = 24;
constexpr unsigned pffftMinimumSize = 32;

} // namespace

FFTFrame::FFTFrame(unsigned fftSize)
    : m_FFTSize(fftSize)
    , m_log2FFTSize(fftSize ? static_cast<unsigned>(std::log2(fftSize)) : 0)
    , m_realData(fftSize / 2)
    , m_imagData(fftSize / 2)
{
    if (fftSize >= pffftMinimumSize && !(fftSize % pffftMinimumSize)) {
        m_setup = pffft_new_setup(fftSize, PFFFT_REAL);
        m_spectrum = static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float)));
        m_work = static_cast<float*>(pffft_aligned_malloc(fftSize * sizeof(float)));
    }
}

FFTFrame::FFTFrame()
    : m_FFTSize(0)
    , m_log2FFTSize(0)
{
}

FFTFrame::FFTFrame(const FFTFrame& frame)
    : FFTFrame(frame.m_FFTSize)
{
    memcpySpan(m_realData.span(), frame.realData().span());
    memcpySpan(m_imagData.span(), frame.imagData().span());
}

FFTFrame::~FFTFrame()
{
    if (m_setup)
        pffft_destroy_setup(m_setup);
    if (m_spectrum)
        pffft_aligned_free(m_spectrum);
    if (m_work)
        pffft_aligned_free(m_work);
}

void FFTFrame::initialize()
{
}

void FFTFrame::doFFT(std::span<const float> data)
{
    unsigned size = m_FFTSize;
    unsigned half = size / 2;
    auto real = m_realData.span();
    auto imag = m_imagData.span();
    if (m_setup) {
        // Ordered output: X0, X(N/2), then Re/Im of X1 .. X(N/2-1).
        memcpy(m_spectrum, data.data(), size * sizeof(float));
        pffft_transform_ordered(m_setup, m_spectrum, m_spectrum, m_work, PFFFT_FORWARD);
        real[0] = m_spectrum[0];
        imag[0] = m_spectrum[1];
        for (unsigned k = 1; k < half; ++k) {
            real[k] = m_spectrum[2 * k];
            imag[k] = m_spectrum[2 * k + 1];
        }
        return;
    }
    // Small sizes: the DFT itself.
    for (unsigned k = 0; k <= half; ++k) {
        double sumReal = 0;
        double sumImag = 0;
        for (unsigned n = 0; n < size; ++n) {
            double angle = -2 * std::numbers::pi * k * n / size;
            sumReal += data[n] * std::cos(angle);
            sumImag += data[n] * std::sin(angle);
        }
        if (!k)
            real[0] = sumReal;
        else if (k == half)
            imag[0] = sumReal; // the Nyquist component is real
        else {
            real[k] = sumReal;
            imag[k] = sumImag;
        }
    }
}

void FFTFrame::doInverseFFT(std::span<float> data)
{
    unsigned size = m_FFTSize;
    unsigned half = size / 2;
    auto real = m_realData.span();
    auto imag = m_imagData.span();
    if (m_setup) {
        m_spectrum[0] = real[0];
        m_spectrum[1] = imag[0];
        for (unsigned k = 1; k < half; ++k) {
            m_spectrum[2 * k] = real[k];
            m_spectrum[2 * k + 1] = imag[k];
        }
        pffft_transform_ordered(m_setup, m_spectrum, m_spectrum, m_work, PFFFT_BACKWARD);
        memcpy(data.data(), m_spectrum, size * sizeof(float));
    } else {
        for (unsigned n = 0; n < size; ++n) {
            double sum = real[0] + imag[0] * ((n % 2) ? -1 : 1);
            for (unsigned k = 1; k < half; ++k) {
                double angle = 2 * std::numbers::pi * k * n / size;
                sum += 2 * (real[k] * std::cos(angle) - imag[k] * std::sin(angle));
            }
            data[n] = sum;
        }
    }
    // So that IFFT(FFT(x)) == x.
    VectorMath::multiplyByScalar(data.first(size), 1.0f / size, data);
}

int FFTFrame::minFFTSize()
{
    return 1 << minimumPow2Size;
}

int FFTFrame::maxFFTSize()
{
    return 1 << maximumPow2Size;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(ZEROG)
