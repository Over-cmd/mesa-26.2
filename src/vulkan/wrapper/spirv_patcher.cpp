#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <cstring>
#include <cstdint>

#include "spirv_patcher.hpp"
#include "wrapper_log.h"

// Magic number estándar de SPIR-V
static constexpr uint32_t SPIRV_MAGIC_NUMBER = 0x07230203;

namespace Decoration {
   static constexpr uint32_t Builtin = 11;
   static constexpr uint32_t RelaxedPrecision = 0;
   static constexpr uint32_t NoPerspective = 5;
   static constexpr uint32_t Flat = 6;

   namespace Literals {
      static constexpr uint32_t ClipDistance = 3;
      static constexpr uint32_t CullDistance = 4;
   }
}

namespace Capability {
   static constexpr uint32_t Matrix = 0;
   static constexpr uint32_t Shader = 1;
   static constexpr uint32_t ClipDistance = 32;
   static constexpr uint32_t CullDistance = 33;
   // Capacidades de 16 bits a menudo problemáticas en Vulkan 1.0/1.1 en Mali G52
   static constexpr uint32_t Float16 = 9;      /* antes 4440, que no es una capacidad de SPIR-V */
   static constexpr uint32_t StorageBuffer16BitAccess = 4433;
   static constexpr uint32_t UniformAndStorageBuffer16BitAccess = 4434;
   static constexpr uint32_t StoragePushConstant16 = 4435;
}

namespace OpCode {
   static constexpr uint32_t OpCapability = 17;
   static constexpr uint32_t OpConstantComposite = 44;
   static constexpr uint32_t OpSpecConstantTrue = 48;
   static constexpr uint32_t OpSpecConstantFalse = 49;
   static constexpr uint32_t OpSpecConstant = 50;
   static constexpr uint32_t OpSpecConstantComposite = 51;
   static constexpr uint32_t OpDecorate = 71;
   static constexpr uint32_t OpMemberDecorate = 72;
}

namespace OpType {
   static constexpr uint32_t OpTypeBool = 20;
   static constexpr uint32_t OpTypeInt = 21;
   static constexpr uint32_t OpTypeFloat = 22;
   static constexpr uint32_t OpTypeVector = 23;
}

#ifdef __cplusplus
extern "C" {
#endif

void
remove_ClipDistance(uint32_t *__restrict__ pCode, size_t *__restrict__ codeSize)
{
   if (!pCode || !codeSize || *codeSize < 5 * sizeof(uint32_t))
      return;

   if (pCode[0] != SPIRV_MAGIC_NUMBER)
      return;

   uint32_t offset = 5;
   size_t totalWords = *codeSize / sizeof(uint32_t);
   
   if (totalWords <= offset)
      return;

   std::vector<uint32_t> code(pCode, pCode + totalWords);
   std::map<uint32_t, uint32_t> instructions;

   while (offset < code.size()) {
      uint32_t instruction = code[offset];
      /* Primera palabra de una instrucción SPIR-V: [31:16] = número de palabras, [15:0] = opcode
         (estaban intercambiados: ninguna instrucción se reconocía y el recorrido se desalineaba) */
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > code.size())
         break;

      if (opcode == OpCode::OpCapability && offset + 1 < code.size()) {
         uint32_t cap = code[offset + 1];
         if (cap == Capability::ClipDistance || cap == Capability::CullDistance) {
            instructions[offset] = length;
         }
      }

      if (opcode == OpCode::OpDecorate && offset + 3 < code.size()) {
         if (code[offset + 2] == Decoration::Builtin) {
            uint32_t literal = code[offset + 3];
            if (literal == Decoration::Literals::ClipDistance || 
                literal == Decoration::Literals::CullDistance) {
               instructions[offset] = length;
            }
         }
      }

      if (opcode == OpCode::OpMemberDecorate && offset + 4 < code.size()) {
         if (code[offset + 3] == Decoration::Builtin) {
            uint32_t literal = code[offset + 4];
            if (literal == Decoration::Literals::ClipDistance || 
                literal == Decoration::Literals::CullDistance) {
               instructions[offset] = length;
            }
         }
      }
      
      offset += length;
   }
 
   if (instructions.empty())
      return;

   for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
      if (it->first + it->second <= code.size()) {
         code.erase(code.begin() + it->first, code.begin() + it->first + it->second);
      }
   }
   
   size_t newByteSize = code.size() * sizeof(uint32_t);
   if (newByteSize <= *codeSize) {
      std::memcpy(pCode, code.data(), newByteSize);
      *codeSize = newByteSize;
      WRAPPER_LOG(info, "spirv_patcher: Removed %zu Clip/CullDistance instructions successfully.", instructions.size());
   } else {
      WRAPPER_LOG(error, "spirv_patcher: New code size exceeds original buffer allocation!");
   }
}

void
remove_unsupported_capabilities_for_mali(uint32_t *__restrict__ pCode, size_t *__restrict__ codeSize)
{
   if (!pCode || !codeSize || *codeSize < 5 * sizeof(uint32_t))
      return;

   if (pCode[0] != SPIRV_MAGIC_NUMBER)
      return;

   uint32_t offset = 5;
   size_t totalWords = *codeSize / sizeof(uint32_t);
   std::vector<uint32_t> code(pCode, pCode + totalWords);
   std::map<uint32_t, uint32_t> instructions;

   /* Si el módulo usa tipos de 16 bits, quitar estas capacidades lo dejaría INVÁLIDO (OpTypeFloat 16 exige Float16,
      el acceso a almacenamiento de 16 bits exige las demás). Solo se eliminan cuando no se usan. */
   for (uint32_t o = 5; o < code.size();) {
      uint32_t len = code[o] >> 16, op = code[o] & 0xffffu;
      if (len == 0 || o + len > code.size()) break;
      if ((op == OpType::OpTypeFloat || op == OpType::OpTypeInt) && o + 2 < code.size() && code[o + 2] == 16) {
         WRAPPER_LOG(info, "spirv_patcher: module uses 16-bit types; 16-bit capabilities are kept.");
         return;
      }
      o += len;
   }

   while (offset < code.size()) {
      uint32_t instruction = code[offset];
      /* Primera palabra de una instrucción SPIR-V: [31:16] = número de palabras, [15:0] = opcode
         (estaban intercambiados: ninguna instrucción se reconocía y el recorrido se desalineaba) */
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > code.size())
         break;

      if (opcode == OpCode::OpCapability && offset + 1 < code.size()) {
         uint32_t cap = code[offset + 1];
         if (cap == Capability::StorageBuffer16BitAccess ||
             cap == Capability::UniformAndStorageBuffer16BitAccess ||
             cap == Capability::StoragePushConstant16 ||
             cap == Capability::Float16) {
            instructions[offset] = length;
         }
      }

      offset += length;
   }

   if (instructions.empty())
      return;

   for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
      if (it->first + it->second <= code.size()) {
         code.erase(code.begin() + it->first, code.begin() + it->first + it->second);
      }
   }

   size_t newByteSize = code.size() * sizeof(uint32_t);
   if (newByteSize <= *codeSize) {
      std::memcpy(pCode, code.data(), newByteSize);
      *codeSize = newByteSize;
      WRAPPER_LOG(info, "spirv_patcher: Stripped %zu unsupported 16-bit/advanced capabilities for Mali G52.", instructions.size());
   }
}

void 
patch_OpConstantComposite_to_OpSpecConstantComposite(uint32_t *__restrict__ pCode, size_t codeSize)
{
   if (!pCode || codeSize < 5 * sizeof(uint32_t))
      return;

   if (pCode[0] != SPIRV_MAGIC_NUMBER)
      return;

   std::unordered_set<uint32_t> bool_constants;
   std::unordered_map<uint32_t, uint32_t> vector_bool_constants;
   std::unordered_set<uint32_t> spec_constants;
   
   uint32_t offset = 5;
   size_t totalWords = codeSize / sizeof(uint32_t);
   size_t patchedCount = 0;

   while (offset < totalWords) {
      uint32_t instruction = pCode[offset];
      /* Primera palabra de una instrucción SPIR-V: [31:16] = número de palabras, [15:0] = opcode
         (estaban intercambiados: ninguna instrucción se reconocía y el recorrido se desalineaba) */
      uint32_t length = instruction >> 16;
      uint32_t opcode = instruction & 0xffffu;

      if (length == 0 || offset + length > totalWords)
         break;

      if (opcode == OpType::OpTypeBool && offset + 1 < totalWords) {
         bool_constants.insert(pCode[offset + 1]);
      }
      else if (opcode == OpType::OpTypeVector && offset + 3 < totalWords) {
         uint32_t variable_id = pCode[offset + 1];
         uint32_t component_type = pCode[offset + 2];
         uint32_t component_number = pCode[offset + 3];
         if (bool_constants.count(component_type)) {
            vector_bool_constants[variable_id] = component_number;
         }
      }
      else if ((opcode == OpCode::OpSpecConstantTrue || 
                opcode == OpCode::OpSpecConstantFalse || 
                opcode == OpCode::OpSpecConstant) && offset + 2 < totalWords) {
         spec_constants.insert(pCode[offset + 2]);
      }
      else if (opcode == OpCode::OpConstantComposite && offset + 1 < totalWords) {
         uint32_t vector_type_id = pCode[offset + 1];
         auto it = vector_bool_constants.find(vector_type_id);
         if (it != vector_bool_constants.end()) {
            uint32_t component_number = it->second;
            uint32_t matches = 0;
            
            for (uint32_t index = 0; index < component_number && (offset + 3 + index) < totalWords; index++) {
               uint32_t component = pCode[offset + 3 + index];
               if (spec_constants.count(component)) {
                  matches++;   	 
               }
            }
            if (matches == component_number) {
               pCode[offset] = (pCode[offset] & 0xffff0000u) | (OpCode::OpSpecConstantComposite & 0xffffu);
               patchedCount++;
            }
         }
      }
         
      offset += length;
   }

   if (patchedCount > 0) {
      WRAPPER_LOG(info, "spirv_patcher: Patched %zu OpConstantComposite to OpSpecConstantComposite.", patchedCount);
   }
}

bool
patch_spirv_shader(uint32_t *__restrict__ pCode, size_t *__restrict__ codeSize)
{
   if (!pCode || !codeSize || *codeSize < 5 * sizeof(uint32_t))
      return false;

   if (pCode[0] != SPIRV_MAGIC_NUMBER)
      return false;

   // Aplicar secuencia completa de parcheo in-place compatible con Mali G52
   remove_ClipDistance(pCode, codeSize);
   remove_unsupported_capabilities_for_mali(pCode, codeSize);
   patch_OpConstantComposite_to_OpSpecConstantComposite(pCode, *codeSize);

   return true;
}

#ifdef __cplusplus
}
#endif
