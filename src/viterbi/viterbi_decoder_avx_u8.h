 /* Generic Viterbi decoder,
 * Copyright Phil Karn, KA9Q,
 * Karn's original code can be found here: https://github.com/ka9q/libfec 
 * May be used under the terms of the GNU Lesser General Public License (LGPL)
 * see http://www.gnu.org/copyleft/lgpl.html
 */
#pragma once
#include "viterbi_decoder_scalar.h"
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <vector>
#include <immintrin.h>

// Vectorisation using AVX
//     8bit integers for errors, soft-decision values
//     32 way vectorisation from 256bits/8bits 
//     64bit decision type since 32 x 2 decisions bits per branch
template <typename absolute_error_t = uint64_t>
class ViterbiDecoder_AVX_u8: public ViterbiDecoder_Scalar<uint8_t, int8_t, uint64_t, absolute_error_t>
{
public:
    static constexpr size_t ALIGN_AMOUNT = sizeof(__m256i);
    static constexpr size_t K_min = 7;
private:
    const size_t m256_width_metric;
    const size_t m256_width_branch_table;
    const size_t u32_width_decision; 
    std::vector<__m256i> m256_symbols;
public:
    // NOTE: branch_table.K >= 7 and branch_table.alignment >= 32  
    template <typename ... U>
    ViterbiDecoder_AVX_u8(U&& ... args)
    :   ViterbiDecoder_Scalar(std::forward<U>(args)...),
        // metric:       NUMSTATES   * sizeof(u8)                       = NUMSTATES
        // branch_table: NUMSTATES/2 * sizeof(s8)                       = NUMSTATES/2  
        // decision:     NUMSTATES/DECISION_BITSIZE * DECISION_BYTESIZE = NUMSTATES/8
        // 
        // m256_metric_width:       NUMSTATES   / sizeof(__m256i) = NUMSTATES/32
        // m256_branch_table_width: NUMSTATES/2 / sizeof(__m256i) = NUMSTATES/64
        // u32_decision_width:      NUMSTATES/8 / sizeof(u64)     = NUMSTATES/64
        m256_width_metric(NUMSTATES/ALIGN_AMOUNT),
        m256_width_branch_table(NUMSTATES/(2u*ALIGN_AMOUNT)),
        u32_width_decision(NUMSTATES/(2u*ALIGN_AMOUNT)),
        m256_symbols(R)
    {
        assert(K >= K_min);
        // Metrics must meet alignment requirements
        assert((NUMSTATES * sizeof(uint8_t)) % ALIGN_AMOUNT == 0);
        assert((NUMSTATES * sizeof(uint8_t)) >= ALIGN_AMOUNT);
        // Branch table must be meet alignment requirements 
        assert(branch_table.alignment % ALIGN_AMOUNT == 0);
        assert(branch_table.alignment >= ALIGN_AMOUNT);

        assert(((uintptr_t)m256_symbols.data() % ALIGN_AMOUNT) == 0);
    }

    virtual void update(const int8_t* symbols, const size_t N) {
        // number of symbols must be a multiple of the code rate
        assert(N % R == 0);
        const size_t total_decoded_bits = N / R;
        assert((total_decoded_bits + curr_decoded_bit) <= decisions.size());

        for (size_t s = 0; s < N; s+=R) {
            auto* decision = get_decision(curr_decoded_bit);
            auto* old_metric = get_old_metric();
            auto* new_metric = get_new_metric();
            simd_bfly(&symbols[s], decision, old_metric, new_metric);
            if (new_metric[0] >= config.renormalisation_threshold) {
                simd_renormalise(new_metric);
            }
            swap_metrics();
            curr_decoded_bit++;
        }
    }
private:
    inline
    void simd_bfly(const int8_t* symbols, uint64_t* decision, uint8_t* old_metric, uint8_t* new_metric) 
    {
        const __m256i* m256_branch_table = reinterpret_cast<const __m256i*>(branch_table.data());
        __m256i* m256_old_metric = reinterpret_cast<__m256i*>(old_metric);
        __m256i* m256_new_metric = reinterpret_cast<__m256i*>(new_metric);

        assert(((uintptr_t)m256_branch_table % ALIGN_AMOUNT) == 0);
        assert(((uintptr_t)m256_old_metric % ALIGN_AMOUNT) == 0);
        assert(((uintptr_t)m256_new_metric % ALIGN_AMOUNT) == 0);

        // Vectorise constants
        for (size_t i = 0; i < R; i++) {
            m256_symbols[i] = _mm256_set1_epi8(symbols[i]);
        }
        const __m256i max_error = _mm256_set1_epi8(config.soft_decision_max_error);

        for (size_t curr_state = 0u; curr_state < m256_width_branch_table; curr_state++) {
            // Total errors across R symbols
            __m256i total_error = _mm256_set1_epi8(0);
            for (size_t i = 0u; i < R; i++) {
                __m256i error = _mm256_subs_epi8(m256_branch_table[i*m256_width_branch_table+curr_state], m256_symbols[i]);
                error = _mm256_abs_epi8(error);
                total_error = _mm256_adds_epu8(total_error, error);
            }

            // Butterfly algorithm
            const __m256i m_total_error = _mm256_subs_epu8(max_error, total_error);
            const __m256i m0 = _mm256_adds_epu8(m256_old_metric[curr_state                      ],   total_error);
            const __m256i m1 = _mm256_adds_epu8(m256_old_metric[curr_state + m256_width_metric/2], m_total_error);
            const __m256i m2 = _mm256_adds_epu8(m256_old_metric[curr_state                      ], m_total_error);
            const __m256i m3 = _mm256_adds_epu8(m256_old_metric[curr_state + m256_width_metric/2],   total_error);
            const __m256i survivor0 = _mm256_min_epu8(m0, m1);
            const __m256i survivor1 = _mm256_min_epu8(m2, m3);
            const __m256i decision0 = _mm256_cmpeq_epi8(survivor0, m1);
            const __m256i decision1 = _mm256_cmpeq_epi8(survivor1, m3);

            // Update metrics
            const __m256i new_metric_lo = _mm256_unpacklo_epi8(survivor0, survivor1);
            const __m256i new_metric_hi = _mm256_unpackhi_epi8(survivor0, survivor1);
            // Reshuffle into correct order along 128bit boundaries
            m256_new_metric[2*curr_state+0] = _mm256_permute2x128_si256(new_metric_lo, new_metric_hi, 0b0010'0000);
            m256_new_metric[2*curr_state+1] = _mm256_permute2x128_si256(new_metric_lo, new_metric_hi, 0b0011'0001);

            // Pack decision bits
            const __m256i shuffled_decision_lo = _mm256_unpacklo_epi8(decision0, decision1);
            const __m256i shuffled_decision_hi = _mm256_unpackhi_epi8(decision0, decision1);
            // Reshuffle into correct order along 128bit boundaries
            const __m256i packed_decision_lo = _mm256_permute2x128_si256(shuffled_decision_lo, shuffled_decision_hi, 0b0010'0000);
            const __m256i packed_decision_hi = _mm256_permute2x128_si256(shuffled_decision_lo, shuffled_decision_hi, 0b0011'0001);
            uint64_t decision_bits_lo = uint64_t(_mm256_movemask_epi8(packed_decision_lo));
            uint64_t decision_bits_hi = uint64_t(_mm256_movemask_epi8(packed_decision_hi));
            // NOTE: mm256_movemask doesn't zero out the upper 32bits
            decision_bits_lo &= uint64_t(0xFFFFFFFF);
            decision_bits_hi &= uint64_t(0xFFFFFFFF);
            decision[curr_state] = uint64_t(decision_bits_hi << 32u) | decision_bits_lo;
        }
    }

    inline
    void simd_renormalise(uint8_t* metric) {
        assert(((uintptr_t)metric % ALIGN_AMOUNT) == 0);
        __m256i* m256_metric = reinterpret_cast<__m256i*>(metric);

        union {
            __m256i m256;
            __m128i m128[2];
            uint8_t u8[32]; 
        } reduce_buffer;

        // Find minimum 
        reduce_buffer.m256 = m256_metric[0];
        for (size_t i = 1u; i < m256_width_metric; i++) {
            reduce_buffer.m256 = _mm256_min_epu8(reduce_buffer.m256, m256_metric[i]);
        }

        // Shift half of the array onto the other half and get the minimum between them
        // Repeat this until we get the minimum value of all 16bit values
        // NOTE: srli performs shift on 128bit lanes
        __m128i adjustv = _mm_min_epu8(reduce_buffer.m128[0], reduce_buffer.m128[1]);
        adjustv = _mm_min_epu8(adjustv, _mm_srli_si128(adjustv, 8));
        adjustv = _mm_min_epu8(adjustv, _mm_srli_si128(adjustv, 4));
        adjustv = _mm_min_epu8(adjustv, _mm_srli_si128(adjustv, 2));
        adjustv = _mm_min_epu8(adjustv, _mm_srli_si128(adjustv, 1));
        reduce_buffer.m128[0] = adjustv;

        // Normalise to minimum
        const uint8_t min = reduce_buffer.u8[0];
        const __m256i vmin = _mm256_set1_epi8(min);
        for (size_t i = 0u; i < m256_width_metric; i++) {
            m256_metric[i] = _mm256_subs_epu8(m256_metric[i], vmin);
        }

        // Keep track of absolute error metrics
        renormalisation_bias += absolute_error_t(min);
    }
};