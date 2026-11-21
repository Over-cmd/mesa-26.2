#!/usr/bin/env python3
# Copyright © 2021 Intel Corporation
# SPDX-License-Identifier: MIT

import argparse
from collections import OrderedDict
from dataclasses import dataclass
import logging
import os
import re
import sys
import typing
import xml.etree.ElementTree as et

import mako
from mako.template import Template

# Añadir el directorio actual al path para garantizar la importación segura del módulo auxiliar
script_dir = os.path.dirname(os.path.abspath(__file__))
if script_dir not in sys.path:
    sys.path.insert(0, script_dir)

# Importación segura con control de errores si falta el módulo auxiliar
try:
    from vk_extensions import Requirements, get_all_required
except ImportError as e:
    print(f"[ERROR] No se pudo importar 'vk_extensions': {e}", file=sys.stderr)
    print("[ERROR] Asegúrate de que 'vk_extensions.py' esté en el mismo directorio o en el PYTHONPATH.", file=sys.stderr)
    sys.exit(1)

# Configuración de logging profesional
logging.basicConfig(level=logging.INFO, format='[%(levelname)s] vk_wrapper_gen: %(message)s')
logger = logging.getLogger(__name__)

COPYRIGHT = u"""/* Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
"""

RENAMED_FEATURES = {
    # See https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/17272#note_1446477 for details
    ('BufferDeviceAddressFeaturesEXT', 'bufferDeviceAddressCaptureReplay'): 'bufferDeviceAddressCaptureReplayEXT',

    ('MeshShaderFeaturesNV', 'taskShader'): 'taskShaderNV',
    ('MeshShaderFeaturesNV', 'meshShader'): 'meshShaderNV',

    ('CooperativeMatrixFeaturesNV', 'cooperativeMatrix'): 'cooperativeMatrixNV',
    ('CooperativeMatrixFeaturesNV', 'cooperativeMatrixRobustBufferAccess'): 'cooperativeMatrixRobustBufferAccessNV',

    ('DeviceGeneratedCommandsFeaturesNV', 'deviceGeneratedCommands'): 'deviceGeneratedCommandsNV',
}

KNOWN_ALIASES = [
    (['Vulkan11Features', '16BitStorageFeatures'], ['storageBuffer16BitAccess', 'uniformAndStorageBuffer16BitAccess', 'storagePushConstant16', 'storageInputOutput16']),
    (['Vulkan11Features', 'MultiviewFeatures'], ['multiview', 'multiviewGeometryShader', 'multiviewTessellationShader']),
    (['Vulkan11Features', 'VariablePointersFeatures'], ['variablePointersStorageBuffer', 'variablePointers']),
    (['Vulkan11Features', 'ProtectedMemoryFeatures'], ['protectedMemory']),
    (['Vulkan11Features', 'SamplerYcbcrConversionFeatures'], ['samplerYcbcrConversion']),
    (['Vulkan11Features', 'ShaderDrawParametersFeatures'], ['shaderDrawParameters']),

    (['Vulkan12Features', '8BitStorageFeatures'], ['storageBuffer8BitAccess', 'uniformAndStorageBuffer8BitAccess', 'storagePushConstant8']),
    (['Vulkan12Features', 'ShaderAtomicInt64Features'], ['shaderBufferInt64Atomics', 'shaderSharedInt64Atomics']),
    (['Vulkan12Features', 'ShaderFloat16Int8Features'], ['shaderFloat16', 'shaderInt8']),
    (
        ['Vulkan12Features', 'DescriptorIndexingFeatures'],
        [
            'shaderInputAttachmentArrayDynamicIndexing',
            'shaderUniformTexelBufferArrayDynamicIndexing',
            'shaderStorageTexelBufferArrayDynamicIndexing',
            'shaderUniformBufferArrayNonUniformIndexing',
            'shaderSampledImageArrayNonUniformIndexing',
            'shaderStorageBufferArrayNonUniformIndexing',
            'shaderStorageImageArrayNonUniformIndexing',
            'shaderInputAttachmentArrayNonUniformIndexing',
            'shaderUniformTexelBufferArrayNonUniformIndexing',
            'shaderStorageTexelBufferArrayNonUniformIndexing',
            'descriptorBindingUniformBufferUpdateAfterBind',
            'descriptorBindingSampledImageUpdateAfterBind',
            'descriptorBindingStorageImageUpdateAfterBind',
            'descriptorBindingStorageBufferUpdateAfterBind',
            'descriptorBindingUniformTexelBufferUpdateAfterBind',
            'descriptorBindingStorageTexelBufferUpdateAfterBind',
            'descriptorBindingUpdateUnusedWhilePending',
            'descriptorBindingPartiallyBound',
            'descriptorBindingVariableDescriptorCount',
            'runtimeDescriptorArray',
        ],
    ),
    (['Vulkan12Features', 'ScalarBlockLayoutFeatures'], ['scalarBlockLayout']),
    (['Vulkan12Features', 'ImagelessFramebufferFeatures'], ['imagelessFramebuffer']),
    (['Vulkan12Features', 'UniformBufferStandardLayoutFeatures'], ['uniformBufferStandardLayout']),
    (['Vulkan12Features', 'ShaderSubgroupExtendedTypesFeatures'], ['shaderSubgroupExtendedTypes']),
    (['Vulkan12Features', 'SeparateDepthStencilLayoutsFeatures'], ['separateDepthStencilLayouts']),
    (['Vulkan12Features', 'HostQueryResetFeatures'], ['hostQueryReset']),
    (['Vulkan12Features', 'TimelineSemaphoreFeatures'], ['timelineSemaphore']),
    (['Vulkan12Features', 'BufferDeviceAddressFeatures', 'BufferDeviceAddressFeaturesEXT'], ['bufferDeviceAddress', 'bufferDeviceAddressMultiDevice']),
    (['Vulkan12Features', 'BufferDeviceAddressFeatures'], ['bufferDeviceAddressCaptureReplay']),
    (['Vulkan12Features', 'VulkanMemoryModelFeatures'], ['vulkanMemoryModel', 'vulkanMemoryModelDeviceScope', 'vulkanMemoryModelAvailabilityVisibilityChains']),

    (['Vulkan13Features', 'ImageRobustnessFeatures'], ['robustImageAccess']),
    (['Vulkan13Features', 'InlineUniformBlockFeatures'], ['inlineUniformBlock', 'descriptorBindingInlineUniformBlockUpdateAfterBind']),
    (['Vulkan13Features', 'PipelineCreationCacheControlFeatures'], ['pipelineCreationCacheControl']),
    (['Vulkan13Features', 'PrivateDataFeatures'], ['privateData']),
    (['Vulkan13Features', 'ShaderDemoteToHelperInvocationFeatures'], ['shaderDemoteToHelperInvocation']),
    (['Vulkan13Features', 'ShaderTerminateInvocationFeatures'], ['shaderTerminateInvocation']),
    (['Vulkan13Features', 'SubgroupSizeControlFeatures'], ['subgroupSizeControl', 'computeFullSubgroups']),
    (['Vulkan13Features', 'Synchronization2Features'], ['synchronization2']),
    (['Vulkan13Features', 'TextureCompressionASTCHDRFeatures'], ['textureCompressionASTC_HDR']),
    (['Vulkan13Features', 'ZeroInitializeWorkgroupMemoryFeatures'], ['shaderZeroInitializeWorkgroupMemory']),
    (['Vulkan13Features', 'DynamicRenderingFeatures'], ['dynamicRendering']),
    (['Vulkan13Features', 'ShaderIntegerDotProductFeatures'], ['shaderIntegerDotProduct']),
    (['Vulkan13Features', 'Maintenance4Features'], ['maintenance4']),
    (['Vulkan14Features', 'GlobalPriorityQueryFeatures'], ['globalPriorityQuery']),
    (
        ['Vulkan14Features', 'ShaderSubgroupRotateFeatures'],
        ['shaderSubgroupRotate', 'shaderSubgroupRotateClustered'],
    ),
    (['Vulkan14Features', 'ShaderFloatControls2Features'], ['shaderFloatControls2']),
    (['Vulkan14Features', 'ShaderExpectAssumeFeatures'], ['shaderExpectAssume']),
    (
        ['Vulkan14Features', 'LineRasterizationFeatures'],
        [
            'rectangularLines',
            'bresenhamLines',
            'smoothLines',
            'stippledRectangularLines',
            'stippledBresenhamLines',
            'stippledSmoothLines',
        ],
    ),
    (
        ['Vulkan14Features', 'VertexAttributeDivisorFeatures'],
        [
            'vertexAttributeInstanceRateDivisor',
            'vertexAttributeInstanceRateZeroDivisor',
        ],
    ),
    (['Vulkan14Features', 'IndexTypeUint8Features'], ['indexTypeUint8']),
    (['Vulkan14Features', 'DynamicRenderingLocalReadFeatures'], ['dynamicRenderingLocalRead']),
    (['Vulkan14Features', 'Maintenance5Features'], ['maintenance5']),
    (['Vulkan14Features', 'Maintenance6Features'], ['maintenance6']),
    (['Vulkan14Features', 'PipelineProtectedAccessFeatures'], ['pipelineProtectedAccess']),
    (['Vulkan14Features', 'PipelineRobustnessFeatures'], ['pipelineRobustness']),
    (['Vulkan14Features', 'HostImageCopyFeatures'], ['hostImageCopy']),
]

# Inicialización segura de alias para evitar duplicados o fallos de aserción silenciosos
for (feature_structs, features) in KNOWN_ALIASES:
    for flag in features:
        for f in feature_structs:
            rename = (f, flag)
            if rename in RENAMED_FEATURES:
                raise ValueError(f"Conflicto detectado: {rename} ya existe en RENAMED_FEATURES")
            RENAMED_FEATURES[rename] = flag

def str_removeprefix(s: str, prefix: str) -> str:
    return s[len(prefix):] if s.startswith(prefix) else s

def get_renamed_feature(c_type: str, feature: str) -> str:
    return RENAMED_FEATURES.get((str_removeprefix(c_type, 'VkPhysicalDevice'), feature), feature)

@dataclass
class FeatureStruct:
    reqs: Requirements
    c_type: str
    s_type: str
    features: typing.List[str]

TEMPLATE_C = Template(COPYRIGHT + """
/* This file generated from ${filename}, don't edit directly. 
 * Target: Mali-G52 / Vulkan 1.0-1.1 Environment
 */

#include "wrapper_private.h"
#include "vk_physical_device_features.h"
#include "vk_util.h"

void
wrapper_setup_device_features(struct wrapper_physical_device *physical_device)
{
   VkPhysicalDevice vk_physical_device = physical_device->dispatch_handle;

   /* Query the device what kind of features are supported. */
   VkPhysicalDeviceFeatures2 supported_features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
   };

% for f in feature_structs:
   ${f.c_type} supported_${f.c_type} = {
      .sType = ${f.s_type},
      .pNext = NULL,
   };
   __vk_append_struct(&supported_features2, &supported_${f.c_type});
% endfor

   physical_device->dispatch_table.GetPhysicalDeviceFeatures2(
      vk_physical_device, &supported_features2);

   vk_set_physical_device_features(&physical_device->vk.supported_features,
                                   &supported_features2);
}
""")

def get_pdev_features(doc: et.Element) -> typing.Optional[typing.List[str]]:
    _type = doc.find(".types/type[@name='VkPhysicalDeviceFeatures']")
    if _type is not None:
        return [p.find('./name').text for p in _type.findall('./member') if p.find('./type').text == 'VkBool32']
    return None

def filter_api_elem(elem: et.Element, api: str) -> bool:
    if 'api' not in elem.attrib:
        return True
    return api in elem.attrib['api'].split(',')

def _parse_version(v: str) -> typing.Tuple[int, int]:
    try:
        parts = v.strip().split('.')
        return (int(parts[0]), int(parts[1]) if len(parts) > 1 else 0)
    except (ValueError, IndexError):
        raise ValueError(f"Versión máxima inválida: '{v}' (use por ejemplo 1.1)")

def get_feature_structs(doc: et.Element, api: str, beta: str, max_version: str) -> typing.List[FeatureStruct]:
    feature_structs = OrderedDict()
    required = get_all_required(doc, 'type', api, beta)
    max_ver = _parse_version(max_version) if max_version else None

    for _type in doc.findall('./types/type[@category="struct"]'):
        struct_extends = _type.attrib.get('structextends', '')
        if not struct_extends:
            continue
        extended_structs = [s.strip() for s in struct_extends.split(',')]
        if 'VkPhysicalDeviceFeatures2' not in extended_structs:
            continue

        typename = _type.attrib.get('name')
        if not typename or typename not in required:
            continue

        # Restricción de versión (p. ej. "1.1" para Mali G52): se descartan las estructuras agregadas VulkanXYFeatures
        # con XY mayor que la versión máxima. (Antes solo se comparaba la cadena literal "1.1": con "1.0" o "1.2" no filtraba bien.)
        m = re.match(r'VkPhysicalDeviceVulkan(\d)(\d)Features$', typename)
        if m and max_ver is not None and (int(m.group(1)), int(m.group(2))) > max_ver:
            continue

        reqs = required[typename]
        guard = reqs.guard
        if guard is not None and (guard != "VK_ENABLE_BETA_EXTENSIONS" or beta.lower() != "true"):
            continue

        s_type = None
        flags = []
        for p in _type:
            if p.tag == 'member':
                if not filter_api_elem(p, api):
                    continue
                m_name = p.find('./name')
                if m_name is not None:
                    name_text = m_name.text
                    if name_text == 'sType':
                        s_type = p.attrib.get('values')
                    elif name_text != 'pNext':
                        type_elem = p.find('./type')
                        if type_elem is not None and type_elem.text == 'VkBool32':
                            flags.append(name_text)

        # Respaldo por si s_type no se halló en los miembros
        if not s_type:
            for elem in _type:
                if "STRUCTURE_TYPE" in str(elem.attrib):
                    s_type = elem.attrib.get('values')

        if not s_type:
            logger.warning(f"{typename}: no se encontró sType; se omite (generaría '.sType = None' y no compilaría)")
            continue

        feature_struct = FeatureStruct(reqs=reqs, c_type=typename, s_type=s_type, features=flags)
        feature_structs[typename] = feature_struct

    return list(feature_structs.values())

def get_feature_structs_from_xml(xml_files: typing.List[str], beta: str, max_version: str, api: str = 'vulkan'):
    diagnostics = []
    pdev_features = None
    feature_structs = []

    for filename in xml_files:
        if not os.path.exists(filename):
            raise FileNotFoundError(f"El archivo XML especificado no existe: {filename}")
        
        try:
            with open(filename, 'r', encoding='utf-8') as f:
                doc = et.parse(f)
        except et.ParseError as e:
            logger.error(f"Error al parsear el XML '{filename}': {e}")
            sys.exit(1)

        feature_structs += get_feature_structs(doc, api, beta, max_version)
        if not pdev_features:
            pdev_features = get_pdev_features(doc)

    unused_renames = {**RENAMED_FEATURES}
    features = OrderedDict()

    if pdev_features:
        for flag in pdev_features:
            features[flag] = 'VkPhysicalDeviceFeatures'

    for f in feature_structs:
        for flag in f.features:
            renamed_flag = get_renamed_feature(f.c_type, flag)
            if renamed_flag not in features:
                features[renamed_flag] = f.c_type
            else:
                a = str_removeprefix(features[renamed_flag], 'VkPhysicalDevice')
                b = str_removeprefix(f.c_type, 'VkPhysicalDevice')
                if (a, flag) not in RENAMED_FEATURES or (b, flag) not in RENAMED_FEATURES:
                    diagnostics.append(f'{a} and {b} both define {flag}')

            unused_renames.pop((str_removeprefix(f.c_type, 'VkPhysicalDevice'), flag), None)

    for rename in unused_renames:
        diagnostics.append(f'unused rename {rename}')

    if diagnostics:
        logger.warning("Se encontraron avisos durante la generación de características:\n" + "\n".join(diagnostics))

    return pdev_features, feature_structs, features

def main():
    parser = argparse.ArgumentParser(description="Generador optimizado de características Vulkan para Mali G52 (Vulkan 1.0/1.1).")
    parser.add_argument('--out-c', required=True, help='Ruta del archivo C de salida.')
    parser.add_argument('--beta', default='false', help='Habilitar extensiones beta ("true"/"false"). Por defecto: false.')
    parser.add_argument('--max-version', default='1.1', help='Versión máxima de Vulkan a soportar (ej. "1.1" para Mali G52).')
    parser.add_argument('--xml',
                        help='Archivo(s) XML de la API Vulkan.',
                        required=True, action='append', dest='xml_files')
    args = parser.parse_args()

    try:
        pdev_features, feature_structs, all_flags = get_feature_structs_from_xml(args.xml_files, args.beta, args.max_version)

        environment = {
            'filename': os.path.basename(__file__),
            'pdev_features': pdev_features,
            'feature_structs': feature_structs,
            'all_flags': all_flags,
            'get_renamed_feature': get_renamed_feature,
        }

        # Asegurar que el directorio de salida exista de manera segura
        out_dir = os.path.dirname(args.out_c)
        if out_dir and not os.path.exists(out_dir):
            os.makedirs(out_dir, exist_ok=True)

        with open(args.out_c, 'w', encoding='utf-8') as f:
            f.write(TEMPLATE_C.render(**environment))
            
        logger.info(f"Archivo de características generado exitosamente para Vulkan <= {args.max_version} en: {args.out_c}")

    except Exception as e:
        logger.error(f"Falló la ejecución del generador: {e}")
        if 'mako' in sys.modules:
            try:
                print(mako.exceptions.text_error_template().render(), file=sys.stderr)
            except Exception:
                pass
        sys.exit(1)

if __name__ == '__main__':
    main()
