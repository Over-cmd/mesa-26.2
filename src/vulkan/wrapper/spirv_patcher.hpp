#ifndef SPIRV_PATCHER_HPP
#define SPIRV_PATCHER_HPP

#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void
remove_ClipDistance(uint32_t *pCode, size_t *codeSize);

void
patch_OpConstantComposite_to_OpSpecConstantComposite(uint32_t *pCode, size_t codeSize);

#ifdef __cplusplus
}
#endif

#endif /* SPIRV_PATCHER_HPP */
