// vk-device.cpp
#include "vk-device.h"

#include "vk-buffer.h"
#include "vk-command-queue.h"
#include "vk-fence.h"
#include "vk-query.h"
#include "vk-render-pass.h"
#include "vk-resource-views.h"
#include "vk-sampler.h"
#include "vk-shader-object.h"
#include "vk-shader-object-layout.h"
#include "vk-shader-program.h"
#include "vk-shader-table.h"
#include "vk-swap-chain.h"
#include "vk-transient-heap.h"
#include "vk-vertex-layout.h"

#include "vk-helper-functions.h"

namespace gfx
{

using namespace Slang;

namespace vk
{

DeviceImpl::~DeviceImpl()
{
    // Check the device queue is valid else, we can't wait on it..
    if (m_deviceQueue.isValid())
    {
        waitForGpu();
    }

    m_shaderObjectLayoutCache = decltype(m_shaderObjectLayoutCache)();
    shaderCache.free();
    m_deviceObjectsWithPotentialBackReferences.clearAndDeallocate();

    if (m_api.vkDestroySampler)
    {
        m_api.vkDestroySampler(m_device, m_defaultSampler, nullptr);
    }

    m_deviceQueue.destroy();

    descriptorSetAllocator.close();

    m_emptyFramebuffer = nullptr;

    if (m_device != VK_NULL_HANDLE)
    {
        if (m_desc.existingDeviceHandles.handles[2].handleValue == 0)
            m_api.vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        if (m_debugReportCallback != VK_NULL_HANDLE)
            m_api.vkDestroyDebugReportCallbackEXT(m_api.m_instance, m_debugReportCallback, nullptr);
        if (m_api.m_instance != VK_NULL_HANDLE &&
            m_desc.existingDeviceHandles.handles[0].handleValue == 0)
            m_api.vkDestroyInstance(m_api.m_instance, nullptr);
    }
}

// TODO: Is "location" still needed for this function?
VkBool32 DeviceImpl::handleDebugMessage(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objType,
    uint64_t srcObject,
    Size location,
    int32_t msgCode,
    const char* pLayerPrefix,
    const char* pMsg)
{
    DebugMessageType msgType = DebugMessageType::Info;

    char const* severity = "message";
    if (flags & VK_DEBUG_REPORT_WARNING_BIT_EXT)
    {
        severity = "warning";
        msgType = DebugMessageType::Warning;
    }
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
    {
        severity = "error";
        msgType = DebugMessageType::Error;
    }

    // pMsg can be really big (it can be assembler dump for example)
    // Use a dynamic buffer to store
    Size bufferSize = strlen(pMsg) + 1 + 1024;
    List<char> bufferArray;
    bufferArray.setCount(bufferSize);
    char* buffer = bufferArray.getBuffer();

    sprintf_s(buffer, bufferSize, "%s: %s %d: %s\n", pLayerPrefix, severity, msgCode, pMsg);

    getDebugCallback()->handleMessage(msgType, DebugMessageSource::Driver, buffer);
    return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DeviceImpl::debugMessageCallback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objType,
    uint64_t srcObject,
    Size location,
    int32_t msgCode,
    const char* pLayerPrefix,
    const char* pMsg,
    void* pUserData)
{
    return ((DeviceImpl*)pUserData)
        ->handleDebugMessage(flags, objType, srcObject, location, msgCode, pLayerPrefix, pMsg);
}

Result DeviceImpl::getNativeDeviceHandles(InteropHandles* outHandles)
{
    outHandles->handles[0].handleValue = (uint64_t)m_api.m_instance;
    outHandles->handles[0].api = InteropHandleAPI::Vulkan;
    outHandles->handles[1].handleValue = (uint64_t)m_api.m_physicalDevice;
    outHandles->handles[1].api = InteropHandleAPI::Vulkan;
    outHandles->handles[2].handleValue = (uint64_t)m_api.m_device;
    outHandles->handles[2].api = InteropHandleAPI::Vulkan;
    return SLANG_OK;
}

Result DeviceImpl::initVulkanInstanceAndDevice(
    const InteropHandle* handles, bool useValidationLayer)
{
    m_features.clear();

    m_queueAllocCount = 0;

    VkInstance instance = VK_NULL_HANDLE;
    if (handles[0].handleValue == 0)
    {
        VkApplicationInfo applicationInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        applicationInfo.pApplicationName = "slang-gfx";
        applicationInfo.pEngineName = "slang-gfx";
        applicationInfo.apiVersion = VK_API_VERSION_1_1;
        applicationInfo.engineVersion = 1;
        applicationInfo.applicationVersion = 1;

        Array<const char*, 6> instanceExtensions;

        instanceExtensions.add(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        instanceExtensions.add(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

        // Software (swiftshader) implementation currently does not support surface extension,
        // so only use it with a hardware implementation.
        if (!m_api.m_module->isSoftware())
        {
            instanceExtensions.add(VK_KHR_SURFACE_EXTENSION_NAME);
            // Note: this extension is not yet supported by nvidia drivers, disable for now.
            // instanceExtensions.add("VK_GOOGLE_surfaceless_query");
#if SLANG_WINDOWS_FAMILY
            instanceExtensions.add(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(SLANG_ENABLE_XLIB)
            instanceExtensions.add(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#if ENABLE_VALIDATION_LAYER
            instanceExtensions.add(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
#endif
        }

        VkInstanceCreateInfo instanceCreateInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceCreateInfo.pApplicationInfo = &applicationInfo;
        instanceCreateInfo.enabledExtensionCount = (uint32_t)instanceExtensions.getCount();
        instanceCreateInfo.ppEnabledExtensionNames = &instanceExtensions[0];

        if (useValidationLayer)
        {
            // Depending on driver version, validation layer may or may not exist.
            // Newer drivers comes with "VK_LAYER_KHRONOS_validation", while older
            // drivers provide only the deprecated
            // "VK_LAYER_LUNARG_standard_validation" layer.
            // We will check what layers are available, and use the newer
            // "VK_LAYER_KHRONOS_validation" layer when possible.
            uint32_t layerCount;
            m_api.vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

            List<VkLayerProperties> availableLayers;
            availableLayers.setCount(layerCount);
            m_api.vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.getBuffer());

            const char* layerNames[] = { nullptr };
            for (auto& layer : availableLayers)
            {
                if (strncmp(
                    layer.layerName,
                    "VK_LAYER_KHRONOS_validation",
                    sizeof("VK_LAYER_KHRONOS_validation")) == 0)
                {
                    layerNames[0] = "VK_LAYER_KHRONOS_validation";
                    break;
                }
            }
            // On older drivers, only "VK_LAYER_LUNARG_standard_validation" exists,
            // so we try to use it if we can't find "VK_LAYER_KHRONOS_validation".
            if (!layerNames[0])
            {
                for (auto& layer : availableLayers)
                {
                    if (strncmp(
                        layer.layerName,
                        "VK_LAYER_LUNARG_standard_validation",
                        sizeof("VK_LAYER_LUNARG_standard_validation")) == 0)
                    {
                        layerNames[0] = "VK_LAYER_LUNARG_standard_validation";
                        break;
                    }
                }
            }
            if (layerNames[0])
            {
                instanceCreateInfo.enabledLayerCount = SLANG_COUNT_OF(layerNames);
                instanceCreateInfo.ppEnabledLayerNames = layerNames;
            }
        }
        uint32_t apiVersionsToTry[] = { VK_API_VERSION_1_2, VK_API_VERSION_1_1, VK_API_VERSION_1_0 };
        for (auto apiVersion : apiVersionsToTry)
        {
            applicationInfo.apiVersion = apiVersion;
            if (m_api.vkCreateInstance(&instanceCreateInfo, nullptr, &instance) == VK_SUCCESS)
            {
                break;
            }
        }
    }
    else
    {
        instance = (VkInstance)handles[0].handleValue;
    }
    if (!instance)
        return SLANG_FAIL;
    SLANG_RETURN_ON_FAIL(m_api.initInstanceProcs(instance));
    if (useValidationLayer && m_api.vkCreateDebugReportCallbackEXT)
    {
        VkDebugReportFlagsEXT debugFlags =
            VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;

        VkDebugReportCallbackCreateInfoEXT debugCreateInfo = {
            VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT };
        debugCreateInfo.pfnCallback = &debugMessageCallback;
        debugCreateInfo.pUserData = this;
        debugCreateInfo.flags = debugFlags;

        SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateDebugReportCallbackEXT(
            instance, &debugCreateInfo, nullptr, &m_debugReportCallback));
    }

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    Index selectedDeviceIndex = 0;
    if (handles[1].handleValue == 0)
    {
        uint32_t numPhysicalDevices = 0;
        SLANG_VK_RETURN_ON_FAIL(
            m_api.vkEnumeratePhysicalDevices(instance, &numPhysicalDevices, nullptr));

        List<VkPhysicalDevice> physicalDevices;
        physicalDevices.setCount(numPhysicalDevices);
        SLANG_VK_RETURN_ON_FAIL(m_api.vkEnumeratePhysicalDevices(
            instance, &numPhysicalDevices, physicalDevices.getBuffer()));

        if (m_desc.adapter)
        {
            selectedDeviceIndex = -1;

            String lowerAdapter = String(m_desc.adapter).toLower();

            for (Index i = 0; i < physicalDevices.getCount(); ++i)
            {
                auto physicalDevice = physicalDevices[i];

                VkPhysicalDeviceProperties basicProps = {};
                m_api.vkGetPhysicalDeviceProperties(physicalDevice, &basicProps);

                String lowerName = String(basicProps.deviceName).toLower();

                if (lowerName.indexOf(lowerAdapter) != Index(-1))
                {
                    selectedDeviceIndex = i;
                    break;
                }
            }
            if (selectedDeviceIndex < 0)
            {
                // Device not found
                return SLANG_FAIL;
            }
        }

        physicalDevice = physicalDevices[selectedDeviceIndex];
    }
    else
    {
        physicalDevice = (VkPhysicalDevice)handles[1].handleValue;
    }

    SLANG_RETURN_ON_FAIL(m_api.initPhysicalDevice(physicalDevice));

    // Obtain the name of the selected adapter.
    {
        VkPhysicalDeviceProperties basicProps = {};
        m_api.vkGetPhysicalDeviceProperties(physicalDevice, &basicProps);
        m_adapterName = basicProps.deviceName;
        m_info.adapterName = m_adapterName.begin();
    }

    List<const char*> deviceExtensions;
    deviceExtensions.add(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pEnabledFeatures = &m_api.m_deviceFeatures;

    // Get the device features (doesn't use, but useful when debugging)
    if (m_api.vkGetPhysicalDeviceFeatures2)
    {
        VkPhysicalDeviceFeatures2 deviceFeatures2 = {};
        deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        m_api.vkGetPhysicalDeviceFeatures2(m_api.m_physicalDevice, &deviceFeatures2);
    }

    VkPhysicalDeviceProperties basicProps = {};
    m_api.vkGetPhysicalDeviceProperties(m_api.m_physicalDevice, &basicProps);

    // Compute timestamp frequency.
    m_info.timestampFrequency = uint64_t(1e9 / basicProps.limits.timestampPeriod);

    // Get the API version
    const uint32_t majorVersion = VK_VERSION_MAJOR(basicProps.apiVersion);
    const uint32_t minorVersion = VK_VERSION_MINOR(basicProps.apiVersion);

    auto& extendedFeatures = m_api.m_extendedFeatures;

    // API version check, can't use vkGetPhysicalDeviceProperties2 yet since this device might not
    // support it
    if (VK_MAKE_VERSION(majorVersion, minorVersion, 0) >= VK_API_VERSION_1_1 &&
        m_api.vkGetPhysicalDeviceProperties2 && m_api.vkGetPhysicalDeviceFeatures2)
    {
        // Get device features
        VkPhysicalDeviceFeatures2 deviceFeatures2 = {};
        deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        // Inline uniform block
        extendedFeatures.inlineUniformBlockFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.inlineUniformBlockFeatures;

        // Buffer device address features
        extendedFeatures.bufferDeviceAddressFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.bufferDeviceAddressFeatures;

        // Ray query features
        extendedFeatures.rayQueryFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.rayQueryFeatures;

        // Ray tracing pipeline features
        extendedFeatures.rayTracingPipelineFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.rayTracingPipelineFeatures;

        // Acceleration structure features
        extendedFeatures.accelerationStructureFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.accelerationStructureFeatures;

        // Subgroup features
        extendedFeatures.shaderSubgroupExtendedTypeFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.shaderSubgroupExtendedTypeFeatures;

        // Extended dynamic states
        extendedFeatures.extendedDynamicStateFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.extendedDynamicStateFeatures;

        // Timeline Semaphore
        extendedFeatures.timelineFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.timelineFeatures;

        // Float16
        extendedFeatures.float16Features.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.float16Features;

        // 16-bit storage
        extendedFeatures.storage16BitFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.storage16BitFeatures;

        // Atomic64
        extendedFeatures.atomicInt64Features.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.atomicInt64Features;

        // robustness2 features
        extendedFeatures.robustness2Features.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.robustness2Features;

        // Atomic Float
        // To detect atomic float we need
        // https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkPhysicalDeviceShaderAtomicFloatFeaturesEXT.html

        extendedFeatures.atomicFloatFeatures.pNext = deviceFeatures2.pNext;
        deviceFeatures2.pNext = &extendedFeatures.atomicFloatFeatures;

        m_api.vkGetPhysicalDeviceFeatures2(m_api.m_physicalDevice, &deviceFeatures2);

        if (deviceFeatures2.features.shaderResourceMinLod)
        {
            m_features.add("shader-resource-min-lod");
        }
        if (deviceFeatures2.features.shaderFloat64)
        {
            m_features.add("double");
        }
        if (deviceFeatures2.features.shaderInt64)
        {
            m_features.add("int64");
        }
        if (deviceFeatures2.features.shaderInt16)
        {
            m_features.add("int16");
        }
        // If we have float16 features then enable
        if (extendedFeatures.float16Features.shaderFloat16)
        {
            // Link into the creation features
            extendedFeatures.float16Features.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.float16Features;

            // Add the Float16 extension
            deviceExtensions.add(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);

            // We have half support
            m_features.add("half");
        }

        if (extendedFeatures.storage16BitFeatures.storageBuffer16BitAccess)
        {
            // Link into the creation features
            extendedFeatures.storage16BitFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.storage16BitFeatures;

            // Add the 16-bit storage extension
            deviceExtensions.add(VK_KHR_16BIT_STORAGE_EXTENSION_NAME);

            // We have half support
            m_features.add("16-bit-storage");
        }

        if (extendedFeatures.atomicInt64Features.shaderBufferInt64Atomics)
        {
            // Link into the creation features
            extendedFeatures.atomicInt64Features.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.atomicInt64Features;

            deviceExtensions.add(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
            m_features.add("atomic-int64");
        }

        if (extendedFeatures.atomicFloatFeatures.shaderBufferFloat32AtomicAdd)
        {
            // Link into the creation features
            extendedFeatures.atomicFloatFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.atomicFloatFeatures;

            deviceExtensions.add(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
            m_features.add("atomic-float");
        }

        if (extendedFeatures.timelineFeatures.timelineSemaphore)
        {
            // Link into the creation features
            extendedFeatures.timelineFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.timelineFeatures;
            deviceExtensions.add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            m_features.add("timeline-semaphore");
        }

        if (extendedFeatures.extendedDynamicStateFeatures.extendedDynamicState)
        {
            // Link into the creation features
            extendedFeatures.extendedDynamicStateFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.extendedDynamicStateFeatures;
            deviceExtensions.add(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
            m_features.add("extended-dynamic-states");
        }

        if (extendedFeatures.shaderSubgroupExtendedTypeFeatures.shaderSubgroupExtendedTypes)
        {
            extendedFeatures.shaderSubgroupExtendedTypeFeatures.pNext =
                (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.shaderSubgroupExtendedTypeFeatures;
            deviceExtensions.add(VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME);
            m_features.add("shader-subgroup-extended-types");
        }

        if (extendedFeatures.accelerationStructureFeatures.accelerationStructure)
        {
            extendedFeatures.accelerationStructureFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.accelerationStructureFeatures;
            deviceExtensions.add(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.add(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            m_features.add("acceleration-structure");
        }

        if (extendedFeatures.rayTracingPipelineFeatures.rayTracingPipeline)
        {
            extendedFeatures.rayTracingPipelineFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.rayTracingPipelineFeatures;
            deviceExtensions.add(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            m_features.add("ray-tracing-pipeline");
        }

        if (extendedFeatures.rayQueryFeatures.rayQuery)
        {
            extendedFeatures.rayQueryFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.rayQueryFeatures;
            deviceExtensions.add(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            m_features.add("ray-query");
            m_features.add("ray-tracing");
            m_features.add("sm_6_6");
        }

        if (extendedFeatures.bufferDeviceAddressFeatures.bufferDeviceAddress)
        {
            extendedFeatures.bufferDeviceAddressFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.bufferDeviceAddressFeatures;
            deviceExtensions.add(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);

            m_features.add("buffer-device-address");
        }

        if (extendedFeatures.inlineUniformBlockFeatures.inlineUniformBlock)
        {
            extendedFeatures.inlineUniformBlockFeatures.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.inlineUniformBlockFeatures;
            deviceExtensions.add(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
            m_features.add("inline-uniform-block");
        }

        if (extendedFeatures.robustness2Features.nullDescriptor)
        {
            extendedFeatures.robustness2Features.pNext = (void*)deviceCreateInfo.pNext;
            deviceCreateInfo.pNext = &extendedFeatures.robustness2Features;
            deviceExtensions.add(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            m_features.add("robustness2");
        }

        VkPhysicalDeviceProperties2 extendedProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
        extendedProps.pNext = &rtProps;
        m_api.vkGetPhysicalDeviceProperties2(m_api.m_physicalDevice, &extendedProps);
        m_api.m_rtProperties = rtProps;

        uint32_t extensionCount = 0;
        m_api.vkEnumerateDeviceExtensionProperties(
            m_api.m_physicalDevice, NULL, &extensionCount, NULL);
        Slang::List<VkExtensionProperties> extensions;
        extensions.setCount(extensionCount);
        m_api.vkEnumerateDeviceExtensionProperties(
            m_api.m_physicalDevice, NULL, &extensionCount, extensions.getBuffer());

        HashSet<String> extensionNames;
        for (const auto& e : extensions)
        {
            extensionNames.Add(e.extensionName);
        }

        if (extensionNames.Contains("VK_KHR_external_memory"))
        {
            deviceExtensions.add(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#if SLANG_WINDOWS_FAMILY
            if (extensionNames.Contains("VK_KHR_external_memory_win32"))
            {
                deviceExtensions.add(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
            }
#endif
            m_features.add("external-memory");
        }
        if (extensionNames.Contains(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME))
        {
            deviceExtensions.add(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
            m_features.add("conservative-rasterization-3");
            m_features.add("conservative-rasterization-2");
            m_features.add("conservative-rasterization-1");
        }
        if (extensionNames.Contains(VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
        {
            deviceExtensions.add(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
            if (extensionNames.Contains(VK_EXT_DEBUG_MARKER_EXTENSION_NAME))
            {
                deviceExtensions.add(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);
            }
        }
        if (extensionNames.Contains(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME))
        {
            deviceExtensions.add(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
        }
    }
    if (m_api.m_module->isSoftware())
    {
        m_features.add("software-device");
    }
    else
    {
        m_features.add("hardware-device");
    }

    m_queueFamilyIndex = m_api.findQueue(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    assert(m_queueFamilyIndex >= 0);

    if (handles[2].handleValue == 0)
    {
        float queuePriority = 0.0f;
        VkDeviceQueueCreateInfo queueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueCreateInfo.queueFamilyIndex = m_queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

        deviceCreateInfo.enabledExtensionCount = uint32_t(deviceExtensions.getCount());
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.getBuffer();

        if (m_api.vkCreateDevice(m_api.m_physicalDevice, &deviceCreateInfo, nullptr, &m_device) !=
            VK_SUCCESS)
            return SLANG_FAIL;
    }
    else
    {
        m_device = (VkDevice)handles[2].handleValue;
    }

    SLANG_RETURN_ON_FAIL(m_api.initDeviceProcs(m_device));

    return SLANG_OK;
}

SlangResult DeviceImpl::initialize(const Desc& desc)
{
    // Initialize device info.
    {
        m_info.apiName = "Vulkan";
        m_info.bindingStyle = BindingStyle::Vulkan;
        m_info.projectionStyle = ProjectionStyle::Vulkan;
        m_info.deviceType = DeviceType::Vulkan;
        static const float kIdentity[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        ::memcpy(m_info.identityProjectionMatrix, kIdentity, sizeof(kIdentity));
    }

    m_desc = desc;

    SLANG_RETURN_ON_FAIL(RendererBase::initialize(desc));
    SlangResult initDeviceResult = SLANG_OK;

    for (int forceSoftware = 0; forceSoftware <= 1; forceSoftware++)
    {
        initDeviceResult = m_module.init(forceSoftware != 0);
        if (initDeviceResult != SLANG_OK)
            continue;
        initDeviceResult = m_api.initGlobalProcs(m_module);
        if (initDeviceResult != SLANG_OK)
            continue;
        descriptorSetAllocator.m_api = &m_api;
        initDeviceResult = initVulkanInstanceAndDevice(
            desc.existingDeviceHandles.handles, ENABLE_VALIDATION_LAYER != 0);
        if (initDeviceResult == SLANG_OK)
            break;
    }
    SLANG_RETURN_ON_FAIL(initDeviceResult);

    {
        VkQueue queue;
        m_api.vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &queue);
        SLANG_RETURN_ON_FAIL(m_deviceQueue.init(m_api, queue, m_queueFamilyIndex));
    }

    SLANG_RETURN_ON_FAIL(slangContext.initialize(
        desc.slang,
        SLANG_SPIRV,
        "sm_5_1",
        makeArray(slang::PreprocessorMacroDesc{ "__VK__", "1" }).getView()));

    // Create default sampler.
    {
        VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        SLANG_VK_RETURN_ON_FAIL(
            m_api.vkCreateSampler(m_device, &samplerInfo, nullptr, &m_defaultSampler));
    }

    // Create empty frame buffer.
    {
        IFramebufferLayout::Desc layoutDesc = {};
        layoutDesc.renderTargetCount = 0;
        layoutDesc.depthStencil = nullptr;
        ComPtr<IFramebufferLayout> layout;
        SLANG_RETURN_ON_FAIL(createFramebufferLayout(layoutDesc, layout.writeRef()));
        IFramebuffer::Desc desc = {};
        desc.layout = layout;
        ComPtr<IFramebuffer> framebuffer;
        SLANG_RETURN_ON_FAIL(createFramebuffer(desc, framebuffer.writeRef()));
        m_emptyFramebuffer = static_cast<FramebufferImpl*>(framebuffer.get());
        m_emptyFramebuffer->m_renderer.breakStrongReference();
    }

    return SLANG_OK;
}

void DeviceImpl::waitForGpu() { m_deviceQueue.flushAndWait(); }

SLANG_NO_THROW const DeviceInfo& SLANG_MCALL DeviceImpl::getDeviceInfo() const { return m_info; }

Result DeviceImpl::createTransientResourceHeap(
    const ITransientResourceHeap::Desc& desc, ITransientResourceHeap** outHeap)
{
    RefPtr<TransientResourceHeapImpl> result = new TransientResourceHeapImpl();
    SLANG_RETURN_ON_FAIL(result->init(desc, this));
    returnComPtr(outHeap, result);
    return SLANG_OK;
}

Result DeviceImpl::createCommandQueue(const ICommandQueue::Desc& desc, ICommandQueue** outQueue)
{
    // Only support one queue for now.
    if (m_queueAllocCount != 0)
        return SLANG_FAIL;
    auto queueFamilyIndex = m_api.findQueue(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    VkQueue vkQueue;
    m_api.vkGetDeviceQueue(m_api.m_device, queueFamilyIndex, 0, &vkQueue);
    RefPtr<CommandQueueImpl> result = new CommandQueueImpl();
    result->init(this, vkQueue, queueFamilyIndex);
    returnComPtr(outQueue, result);
    m_queueAllocCount++;
    return SLANG_OK;
}

Result DeviceImpl::createSwapchain(
    const ISwapchain::Desc& desc, WindowHandle window, ISwapchain** outSwapchain)
{
#if !defined(SLANG_ENABLE_XLIB)
    if (window.type == WindowHandle::Type::XLibHandle)
    {
        return SLANG_FAIL;
    }
#endif

    RefPtr<SwapchainImpl> sc = new SwapchainImpl();
    SLANG_RETURN_ON_FAIL(sc->init(this, desc, window));
    returnComPtr(outSwapchain, sc);
    return SLANG_OK;
}

Result DeviceImpl::createFramebufferLayout(
    const IFramebufferLayout::Desc& desc, IFramebufferLayout** outLayout)
{
    RefPtr<FramebufferLayoutImpl> layout = new FramebufferLayoutImpl();
    SLANG_RETURN_ON_FAIL(layout->init(this, desc));
    returnComPtr(outLayout, layout);
    return SLANG_OK;
}

Result DeviceImpl::createRenderPassLayout(
    const IRenderPassLayout::Desc& desc, IRenderPassLayout** outRenderPassLayout)
{
    RefPtr<RenderPassLayoutImpl> result = new RenderPassLayoutImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, desc));
    returnComPtr(outRenderPassLayout, result);
    return SLANG_OK;
}

Result DeviceImpl::createFramebuffer(const IFramebuffer::Desc& desc, IFramebuffer** outFramebuffer)
{
    RefPtr<FramebufferImpl> fb = new FramebufferImpl();
    SLANG_RETURN_ON_FAIL(fb->init(this, desc));
    returnComPtr(outFramebuffer, fb);
    return SLANG_OK;
}

SlangResult DeviceImpl::readTextureResource(
    ITextureResource* texture,
    ResourceState state,
    ISlangBlob** outBlob,
    Size* outRowPitch,
    Size* outPixelSize)
{
    auto textureImpl = static_cast<TextureResourceImpl*>(texture);
    RefPtr<ListBlob> blob = new ListBlob();

    auto desc = textureImpl->getDesc();
    auto width = desc->size.width;
    auto height = desc->size.height;
    FormatInfo sizeInfo;
    SLANG_RETURN_ON_FAIL(gfxGetFormatInfo(desc->format, &sizeInfo));
    Size pixelSize = sizeInfo.blockSizeInBytes / sizeInfo.pixelsPerBlock;
    Size rowPitch = width * pixelSize;

    List<TextureResource::Extents> mipSizes;

    const int numMipMaps = desc->numMipLevels;
    auto arraySize = calcEffectiveArraySize(*desc);

    // Calculate how large the buffer has to be
    Size bufferSize = 0;
    // Calculate how large an array entry is
    for (int j = 0; j < numMipMaps; ++j)
    {
        const TextureResource::Extents mipSize = calcMipSize(desc->size, j);

        auto rowSizeInBytes = calcRowSize(desc->format, mipSize.width);
        auto numRows = calcNumRows(desc->format, mipSize.height);

        mipSizes.add(mipSize);

        bufferSize += (rowSizeInBytes * numRows) * mipSize.depth;
    }
    // Calculate the total size taking into account the array
    bufferSize *= arraySize;
    // TODO: Change Index to Count?
    blob->m_data.setCount(Index(bufferSize));

    VKBufferHandleRAII staging;
    SLANG_RETURN_ON_FAIL(staging.init(
        m_api,
        bufferSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    VkCommandBuffer commandBuffer = m_deviceQueue.getCommandBuffer();
    VkImage srcImage = textureImpl->m_image;
    VkImageLayout srcImageLayout = VulkanUtil::getImageLayoutFromState(state);

    Offset dstOffset = 0;
    for (int i = 0; i < arraySize; ++i)
    {
        for (Index j = 0; j < mipSizes.getCount(); ++j)
        {
            const auto& mipSize = mipSizes[j];

            auto rowSizeInBytes = calcRowSize(desc->format, mipSize.width);
            auto numRows = calcNumRows(desc->format, mipSize.height);

            VkBufferImageCopy region = {};

            region.bufferOffset = dstOffset;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;

            region.imageSubresource.aspectMask = getAspectMaskFromFormat(VulkanUtil::getVkFormat(desc->format));
            region.imageSubresource.mipLevel = uint32_t(j);
            region.imageSubresource.baseArrayLayer = i;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = {
                uint32_t(mipSize.width), uint32_t(mipSize.height), uint32_t(mipSize.depth) };

            m_api.vkCmdCopyImageToBuffer(
                commandBuffer, srcImage, srcImageLayout, staging.m_buffer, 1, &region);

            dstOffset += rowSizeInBytes * numRows * mipSize.depth;
        }
    }

    m_deviceQueue.flushAndWait();

    // Write out the data from the buffer
    void* mappedData = nullptr;
    SLANG_RETURN_ON_FAIL(
        m_api.vkMapMemory(m_device, staging.m_memory, 0, bufferSize, 0, &mappedData));

    ::memcpy(blob->m_data.getBuffer(), mappedData, bufferSize);
    m_api.vkUnmapMemory(m_device, staging.m_memory);

    *outPixelSize = pixelSize;
    *outRowPitch = rowPitch;
    returnComPtr(outBlob, blob);
    return SLANG_OK;
}

SlangResult DeviceImpl::readBufferResource(
    IBufferResource* inBuffer, Offset offset, Size size, ISlangBlob** outBlob)
{
    BufferResourceImpl* buffer = static_cast<BufferResourceImpl*>(inBuffer);

    RefPtr<ListBlob> blob = new ListBlob();
    blob->m_data.setCount(size);

    // create staging buffer
    VKBufferHandleRAII staging;

    SLANG_RETURN_ON_FAIL(staging.init(
        m_api,
        size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    // Copy from real buffer to staging buffer
    VkCommandBuffer commandBuffer = m_deviceQueue.getCommandBuffer();

    VkBufferCopy copyInfo = {};
    copyInfo.size = size;
    copyInfo.srcOffset = offset;
    m_api.vkCmdCopyBuffer(commandBuffer, buffer->m_buffer.m_buffer, staging.m_buffer, 1, &copyInfo);

    m_deviceQueue.flushAndWait();

    // Write out the data from the buffer
    void* mappedData = nullptr;
    SLANG_RETURN_ON_FAIL(m_api.vkMapMemory(m_device, staging.m_memory, 0, size, 0, &mappedData));

    ::memcpy(blob->m_data.getBuffer(), mappedData, size);
    m_api.vkUnmapMemory(m_device, staging.m_memory);

    returnComPtr(outBlob, blob);
    return SLANG_OK;
}

Result DeviceImpl::getAccelerationStructurePrebuildInfo(
    const IAccelerationStructure::BuildInputs& buildInputs,
    IAccelerationStructure::PrebuildInfo* outPrebuildInfo)
{
    if (!m_api.vkGetAccelerationStructureBuildSizesKHR)
    {
        return SLANG_E_NOT_AVAILABLE;
    }
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    AccelerationStructureBuildGeometryInfoBuilder geomInfoBuilder;
    SLANG_RETURN_ON_FAIL(geomInfoBuilder.build(buildInputs, getDebugCallback()));
    m_api.vkGetAccelerationStructureBuildSizesKHR(
        m_api.m_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &geomInfoBuilder.buildInfo,
        geomInfoBuilder.primitiveCounts.getBuffer(),
        &sizeInfo);
    outPrebuildInfo->resultDataMaxSize = (Size)sizeInfo.accelerationStructureSize;
    outPrebuildInfo->scratchDataSize = (Size)sizeInfo.buildScratchSize;
    outPrebuildInfo->updateScratchDataSize = (Size)sizeInfo.updateScratchSize;
    return SLANG_OK;
}

Result DeviceImpl::createAccelerationStructure(
    const IAccelerationStructure::CreateDesc& desc, IAccelerationStructure** outAS)
{
    if (!m_api.vkCreateAccelerationStructureKHR)
    {
        return SLANG_E_NOT_AVAILABLE;
    }
    RefPtr<AccelerationStructureImpl> resultAS = new AccelerationStructureImpl();
    resultAS->m_offset = desc.offset;
    resultAS->m_size = desc.size;
    resultAS->m_buffer = static_cast<BufferResourceImpl*>(desc.buffer);
    resultAS->m_device = this;
    resultAS->m_desc.type = IResourceView::Type::AccelerationStructure;
    VkAccelerationStructureCreateInfoKHR createInfo = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    createInfo.buffer = resultAS->m_buffer->m_buffer.m_buffer;
    createInfo.offset = desc.offset;
    createInfo.size = desc.size;
    switch (desc.kind)
    {
    case IAccelerationStructure::Kind::BottomLevel:
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        break;
    case IAccelerationStructure::Kind::TopLevel:
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        break;
    default:
        getDebugCallback()->handleMessage(
            DebugMessageType::Error,
            DebugMessageSource::Layer,
            "invalid value of IAccelerationStructure::Kind encountered in desc.kind");
        return SLANG_E_INVALID_ARG;
    }

    SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateAccelerationStructureKHR(
        m_api.m_device, &createInfo, nullptr, &resultAS->m_vkHandle));
    returnComPtr(outAS, resultAS);
    return SLANG_OK;
}

void DeviceImpl::_transitionImageLayout(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkFormat format,
    const TextureResource::Desc& desc,
    VkImageLayout oldLayout,
    VkImageLayout newLayout)
{
    if (oldLayout == newLayout)
        return;

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;

    barrier.subresourceRange.aspectMask = getAspectMaskFromFormat(format);

    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = desc.numMipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    barrier.srcAccessMask = calcAccessFlagsFromImageLayout(oldLayout);
    barrier.dstAccessMask = calcAccessFlagsFromImageLayout(newLayout);

    VkPipelineStageFlags sourceStage = calcPipelineStageFlagsFromImageLayout(oldLayout);
    VkPipelineStageFlags destinationStage = calcPipelineStageFlagsFromImageLayout(newLayout);

    m_api.vkCmdPipelineBarrier(
        commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint32_t DeviceImpl::getQueueFamilyIndex(ICommandQueue::QueueType queueType)
{
    switch (queueType)
    {
    case ICommandQueue::QueueType::Graphics:
    default:
        return m_queueFamilyIndex;
    }
}

void DeviceImpl::_transitionImageLayout(
    VkImage image,
    VkFormat format,
    const TextureResource::Desc& desc,
    VkImageLayout oldLayout,
    VkImageLayout newLayout)
{
    VkCommandBuffer commandBuffer = m_deviceQueue.getCommandBuffer();
    _transitionImageLayout(commandBuffer, image, format, desc, oldLayout, newLayout);
}

Result DeviceImpl::getTextureAllocationInfo(
    const ITextureResource::Desc& descIn, Size* outSize, Size* outAlignment)
{
    TextureResource::Desc desc = fixupTextureDesc(descIn);

    const VkFormat format = VulkanUtil::getVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED)
    {
        assert(!"Unhandled image format");
        return SLANG_FAIL;
    }
    const int arraySize = calcEffectiveArraySize(desc);

    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    switch (desc.type)
    {
    case IResource::Type::Texture1D:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_1D;
        imageInfo.extent = VkExtent3D{ uint32_t(descIn.size.width), 1, 1 };
        break;
    }
    case IResource::Type::Texture2D:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent =
            VkExtent3D{ uint32_t(descIn.size.width), uint32_t(descIn.size.height), 1 };
        break;
    }
    case IResource::Type::TextureCube:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent =
            VkExtent3D{ uint32_t(descIn.size.width), uint32_t(descIn.size.height), 1 };
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        break;
    }
    case IResource::Type::Texture3D:
    {
        // Can't have an array and 3d texture
        assert(desc.arraySize <= 1);

        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.extent = VkExtent3D{
            uint32_t(descIn.size.width),
            uint32_t(descIn.size.height),
            uint32_t(descIn.size.depth) };
        break;
    }
    default:
    {
        assert(!"Unhandled type");
        return SLANG_FAIL;
    }
    }

    imageInfo.mipLevels = desc.numMipLevels;
    imageInfo.arrayLayers = arraySize;

    imageInfo.format = format;

    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = _calcImageUsageFlags(desc.allowedStates, desc.memoryType, nullptr);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    imageInfo.samples = (VkSampleCountFlagBits)desc.sampleDesc.numSamples;

    VkImage image;
    SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateImage(m_device, &imageInfo, nullptr, &image));

    VkMemoryRequirements memRequirements;
    m_api.vkGetImageMemoryRequirements(m_device, image, &memRequirements);

    *outSize = (Size)memRequirements.size;
    *outAlignment = (Size)memRequirements.alignment;

    m_api.vkDestroyImage(m_device, image, nullptr);
    return SLANG_OK;
}

Result DeviceImpl::getTextureRowAlignment(Size* outAlignment)
{
    *outAlignment = 1;
    return SLANG_OK;
}

Result DeviceImpl::createTextureResource(
    const ITextureResource::Desc& descIn,
    const ITextureResource::SubresourceData* initData,
    ITextureResource** outResource)
{
    TextureResource::Desc desc = fixupTextureDesc(descIn);

    const VkFormat format = VulkanUtil::getVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED)
    {
        assert(!"Unhandled image format");
        return SLANG_FAIL;
    }

    const int arraySize = calcEffectiveArraySize(desc);

    RefPtr<TextureResourceImpl> texture(new TextureResourceImpl(desc, this));
    texture->m_vkformat = format;
    // Create the image

    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    switch (desc.type)
    {
    case IResource::Type::Texture1D:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_1D;
        imageInfo.extent = VkExtent3D{ uint32_t(descIn.size.width), 1, 1 };
        break;
    }
    case IResource::Type::Texture2D:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent =
            VkExtent3D{ uint32_t(descIn.size.width), uint32_t(descIn.size.height), 1 };
        break;
    }
    case IResource::Type::TextureCube:
    {
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent =
            VkExtent3D{ uint32_t(descIn.size.width), uint32_t(descIn.size.height), 1 };
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        break;
    }
    case IResource::Type::Texture3D:
    {
        // Can't have an array and 3d texture
        assert(desc.arraySize <= 1);

        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.extent = VkExtent3D{
            uint32_t(descIn.size.width),
            uint32_t(descIn.size.height),
            uint32_t(descIn.size.depth) };
        break;
    }
    default:
    {
        assert(!"Unhandled type");
        return SLANG_FAIL;
    }
    }

    imageInfo.mipLevels = desc.numMipLevels;
    imageInfo.arrayLayers = arraySize;

    imageInfo.format = format;

    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = _calcImageUsageFlags(desc.allowedStates, desc.memoryType, initData);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    imageInfo.samples = (VkSampleCountFlagBits)desc.sampleDesc.numSamples;

    VkExternalMemoryImageCreateInfo externalMemoryImageCreateInfo = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
#if SLANG_WINDOWS_FAMILY
    VkExternalMemoryHandleTypeFlags extMemoryHandleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    if (descIn.isShared)
    {
        externalMemoryImageCreateInfo.pNext = nullptr;
        externalMemoryImageCreateInfo.handleTypes = extMemoryHandleType;
        imageInfo.pNext = &externalMemoryImageCreateInfo;
    }
#endif
    SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateImage(m_device, &imageInfo, nullptr, &texture->m_image));

    VkMemoryRequirements memRequirements;
    m_api.vkGetImageMemoryRequirements(m_device, texture->m_image, &memRequirements);

    // Allocate the memory
    VkMemoryPropertyFlags reqMemoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int memoryTypeIndex =
        m_api.findMemoryTypeIndex(memRequirements.memoryTypeBits, reqMemoryProperties);
    assert(memoryTypeIndex >= 0);

    VkMemoryPropertyFlags actualMemoryProperites =
        m_api.m_deviceMemoryProperties.memoryTypes[memoryTypeIndex].propertyFlags;
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
#if SLANG_WINDOWS_FAMILY
    VkExportMemoryWin32HandleInfoKHR exportMemoryWin32HandleInfo = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    VkExportMemoryAllocateInfoKHR exportMemoryAllocateInfo = {
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR };
    if (descIn.isShared)
    {
        exportMemoryWin32HandleInfo.pNext = nullptr;
        exportMemoryWin32HandleInfo.pAttributes = nullptr;
        exportMemoryWin32HandleInfo.dwAccess =
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
        exportMemoryWin32HandleInfo.name = NULL;

        exportMemoryAllocateInfo.pNext =
            extMemoryHandleType & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
            ? &exportMemoryWin32HandleInfo
            : nullptr;
        exportMemoryAllocateInfo.handleTypes = extMemoryHandleType;
        allocInfo.pNext = &exportMemoryAllocateInfo;
    }
#endif
    SLANG_VK_RETURN_ON_FAIL(
        m_api.vkAllocateMemory(m_device, &allocInfo, nullptr, &texture->m_imageMemory));

    // Bind the memory to the image
    m_api.vkBindImageMemory(m_device, texture->m_image, texture->m_imageMemory, 0);

    VKBufferHandleRAII uploadBuffer;
    if (initData)
    {
        List<TextureResource::Extents> mipSizes;

        VkCommandBuffer commandBuffer = m_deviceQueue.getCommandBuffer();

        const int numMipMaps = desc.numMipLevels;

        // Calculate how large the buffer has to be
        Size bufferSize = 0;
        // Calculate how large an array entry is
        for (int j = 0; j < numMipMaps; ++j)
        {
            const TextureResource::Extents mipSize = calcMipSize(desc.size, j);

            auto rowSizeInBytes = calcRowSize(desc.format, mipSize.width);
            auto numRows = calcNumRows(desc.format, mipSize.height);

            mipSizes.add(mipSize);

            bufferSize += (rowSizeInBytes * numRows) * mipSize.depth;
        }

        // Calculate the total size taking into account the array
        bufferSize *= arraySize;

        SLANG_RETURN_ON_FAIL(uploadBuffer.init(
            m_api,
            bufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

        assert(mipSizes.getCount() == numMipMaps);

        // Copy into upload buffer
        {
            int subResourceCounter = 0;

            uint8_t* dstData;
            m_api.vkMapMemory(m_device, uploadBuffer.m_memory, 0, bufferSize, 0, (void**)&dstData);
            uint8_t* dstDataStart;
            dstDataStart = dstData;

            Offset dstSubresourceOffset = 0;
            for (int i = 0; i < arraySize; ++i)
            {
                for (Index j = 0; j < mipSizes.getCount(); ++j)
                {
                    const auto& mipSize = mipSizes[j];

                    int subResourceIndex = subResourceCounter++;
                    auto initSubresource = initData[subResourceIndex];

                    const ptrdiff_t srcRowStride = (ptrdiff_t)initSubresource.strideY;
                    const ptrdiff_t srcLayerStride = (ptrdiff_t)initSubresource.strideZ;

                    auto dstRowSizeInBytes = calcRowSize(desc.format, mipSize.width);
                    auto numRows = calcNumRows(desc.format, mipSize.height);
                    auto dstLayerSizeInBytes = dstRowSizeInBytes * numRows;

                    const uint8_t* srcLayer = (const uint8_t*)initSubresource.data;
                    uint8_t* dstLayer = dstData + dstSubresourceOffset;

                    for (int k = 0; k < mipSize.depth; k++)
                    {
                        const uint8_t* srcRow = srcLayer;
                        uint8_t* dstRow = dstLayer;

                        for (GfxCount l = 0; l < numRows; l++)
                        {
                            ::memcpy(dstRow, srcRow, dstRowSizeInBytes);

                            dstRow += dstRowSizeInBytes;
                            srcRow += srcRowStride;
                        }

                        dstLayer += dstLayerSizeInBytes;
                        srcLayer += srcLayerStride;
                    }

                    dstSubresourceOffset += dstLayerSizeInBytes * mipSize.depth;
                }
            }

            m_api.vkUnmapMemory(m_device, uploadBuffer.m_memory);
        }

        _transitionImageLayout(
            texture->m_image,
            format,
            *texture->getDesc(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        {
            Offset srcOffset = 0;
            for (int i = 0; i < arraySize; ++i)
            {
                for (Index j = 0; j < mipSizes.getCount(); ++j)
                {
                    const auto& mipSize = mipSizes[j];

                    auto rowSizeInBytes = calcRowSize(desc.format, mipSize.width);
                    auto numRows = calcNumRows(desc.format, mipSize.height);

                    // https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkBufferImageCopy.html
                    // bufferRowLength and bufferImageHeight specify the data in buffer memory as a
                    // subregion of a larger two- or three-dimensional image, and control the
                    // addressing calculations of data in buffer memory. If either of these values
                    // is zero, that aspect of the buffer memory is considered to be tightly packed
                    // according to the imageExtent.

                    VkBufferImageCopy region = {};

                    region.bufferOffset = srcOffset;
                    region.bufferRowLength = 0; // rowSizeInBytes;
                    region.bufferImageHeight = 0;

                    region.imageSubresource.aspectMask = getAspectMaskFromFormat(format);
                    region.imageSubresource.mipLevel = uint32_t(j);
                    region.imageSubresource.baseArrayLayer = i;
                    region.imageSubresource.layerCount = 1;
                    region.imageOffset = { 0, 0, 0 };
                    region.imageExtent = {
                        uint32_t(mipSize.width), uint32_t(mipSize.height), uint32_t(mipSize.depth) };

                    // Do the copy (do all depths in a single go)
                    m_api.vkCmdCopyBufferToImage(
                        commandBuffer,
                        uploadBuffer.m_buffer,
                        texture->m_image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1,
                        &region);

                    // Next
                    srcOffset += rowSizeInBytes * numRows * mipSize.depth;
                }
            }
        }
        auto defaultLayout = VulkanUtil::getImageLayoutFromState(desc.defaultState);
        _transitionImageLayout(
            texture->m_image,
            format,
            *texture->getDesc(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            defaultLayout);
    }
    else
    {
        auto defaultLayout = VulkanUtil::getImageLayoutFromState(desc.defaultState);
        if (defaultLayout != VK_IMAGE_LAYOUT_UNDEFINED)
        {
            _transitionImageLayout(
                texture->m_image,
                format,
                *texture->getDesc(),
                VK_IMAGE_LAYOUT_UNDEFINED,
                defaultLayout);
        }
    }
    m_deviceQueue.flushAndWait();
    returnComPtr(outResource, texture);
    return SLANG_OK;
}

Result DeviceImpl::createBufferResource(
    const IBufferResource::Desc& descIn, const void* initData, IBufferResource** outResource)
{
    BufferResource::Desc desc = fixupBufferDesc(descIn);

    const Size bufferSize = desc.sizeInBytes;

    VkMemoryPropertyFlags reqMemoryProperties = 0;

    VkBufferUsageFlags usage = _calcBufferUsageFlags(desc.allowedStates);
    if (m_api.m_extendedFeatures.bufferDeviceAddressFeatures.bufferDeviceAddress)
    {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (desc.allowedStates.contains(ResourceState::ShaderResource) &&
        m_api.m_extendedFeatures.accelerationStructureFeatures.accelerationStructure)
    {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    if (initData)
    {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    if (desc.allowedStates.contains(ResourceState::ConstantBuffer) ||
        desc.memoryType == MemoryType::Upload || desc.memoryType == MemoryType::ReadBack)
    {
        reqMemoryProperties =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    RefPtr<BufferResourceImpl> buffer(new BufferResourceImpl(desc, this));
    if (desc.isShared)
    {
        SLANG_RETURN_ON_FAIL(buffer->m_buffer.init(
            m_api,
            desc.sizeInBytes,
            usage,
            reqMemoryProperties,
            desc.isShared,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT));
    }
    else
    {
        SLANG_RETURN_ON_FAIL(
            buffer->m_buffer.init(m_api, desc.sizeInBytes, usage, reqMemoryProperties));
    }

    if (initData)
    {
        if (desc.memoryType == MemoryType::DeviceLocal)
        {
            SLANG_RETURN_ON_FAIL(buffer->m_uploadBuffer.init(
                m_api,
                bufferSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
            // Copy into staging buffer
            void* mappedData = nullptr;
            SLANG_VK_CHECK(m_api.vkMapMemory(
                m_device, buffer->m_uploadBuffer.m_memory, 0, bufferSize, 0, &mappedData));
            ::memcpy(mappedData, initData, bufferSize);
            m_api.vkUnmapMemory(m_device, buffer->m_uploadBuffer.m_memory);

            // Copy from staging buffer to real buffer
            VkCommandBuffer commandBuffer = m_deviceQueue.getCommandBuffer();

            VkBufferCopy copyInfo = {};
            copyInfo.size = bufferSize;
            m_api.vkCmdCopyBuffer(
                commandBuffer,
                buffer->m_uploadBuffer.m_buffer,
                buffer->m_buffer.m_buffer,
                1,
                &copyInfo);
            m_deviceQueue.flush();
        }
        else
        {
            // Copy into mapped buffer directly
            void* mappedData = nullptr;
            SLANG_VK_CHECK(m_api.vkMapMemory(
                m_device, buffer->m_buffer.m_memory, 0, bufferSize, 0, &mappedData));
            ::memcpy(mappedData, initData, bufferSize);
            m_api.vkUnmapMemory(m_device, buffer->m_buffer.m_memory);
        }
    }

    returnComPtr(outResource, buffer);
    return SLANG_OK;
}

Result DeviceImpl::createBufferFromNativeHandle(
    InteropHandle handle, const IBufferResource::Desc& srcDesc, IBufferResource** outResource)
{
    RefPtr<BufferResourceImpl> buffer(new BufferResourceImpl(srcDesc, this));

    if (handle.api == InteropHandleAPI::Vulkan)
    {
        buffer->m_buffer.m_buffer = (VkBuffer)handle.handleValue;
    }
    else
    {
        return SLANG_FAIL;
    }

    returnComPtr(outResource, buffer);
    return SLANG_OK;
}

Result DeviceImpl::createSamplerState(ISamplerState::Desc const& desc, ISamplerState** outSampler)
{
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    samplerInfo.magFilter = VulkanUtil::translateFilterMode(desc.minFilter);
    samplerInfo.minFilter = VulkanUtil::translateFilterMode(desc.magFilter);

    samplerInfo.addressModeU = VulkanUtil::translateAddressingMode(desc.addressU);
    samplerInfo.addressModeV = VulkanUtil::translateAddressingMode(desc.addressV);
    samplerInfo.addressModeW = VulkanUtil::translateAddressingMode(desc.addressW);

    samplerInfo.anisotropyEnable = desc.maxAnisotropy > 1;
    samplerInfo.maxAnisotropy = (float)desc.maxAnisotropy;

    // TODO: support translation of border color...
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = desc.reductionOp == TextureReductionOp::Comparison;
    samplerInfo.compareOp = VulkanUtil::translateComparisonFunc(desc.comparisonFunc);
    samplerInfo.mipmapMode = VulkanUtil::translateMipFilterMode(desc.mipFilter);
    samplerInfo.minLod = Math::Max(0.0f, desc.minLOD);
    samplerInfo.maxLod = Math::Clamp(desc.maxLOD, samplerInfo.minLod, VK_LOD_CLAMP_NONE);

    VkSamplerReductionModeCreateInfo reductionInfo = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO };
    reductionInfo.reductionMode = VulkanUtil::translateReductionOp(desc.reductionOp);
    samplerInfo.pNext = &reductionInfo;

    VkSampler sampler;
    SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateSampler(m_device, &samplerInfo, nullptr, &sampler));

    RefPtr<SamplerStateImpl> samplerImpl = new SamplerStateImpl(this);
    samplerImpl->m_sampler = sampler;
    returnComPtr(outSampler, samplerImpl);
    return SLANG_OK;
}

Result DeviceImpl::createTextureView(
    ITextureResource* texture, IResourceView::Desc const& desc, IResourceView** outView)
{
    auto resourceImpl = static_cast<TextureResourceImpl*>(texture);
    RefPtr<TextureResourceViewImpl> view = new TextureResourceViewImpl(this);
    view->m_texture = resourceImpl;
    view->m_desc = desc;
    if (!texture)
    {
        view->m_view = VK_NULL_HANDLE;
        returnComPtr(outView, view);
        return SLANG_OK;
    }

    bool isArray = desc.subresourceRange.layerCount > 1;
    VkImageViewCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.flags = 0;
    createInfo.format = gfxIsTypelessFormat(texture->getDesc()->format)
        ? VulkanUtil::getVkFormat(desc.format)
        : resourceImpl->m_vkformat;
    createInfo.image = resourceImpl->m_image;
    createInfo.components = VkComponentMapping{
        VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A };
    switch (resourceImpl->getType())
    {
    case IResource::Type::Texture1D:
        createInfo.viewType = isArray ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        break;
    case IResource::Type::Texture2D:
        createInfo.viewType = isArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        break;
    case IResource::Type::Texture3D:
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        break;
    case IResource::Type::TextureCube:
        createInfo.viewType = isArray ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    default:
        SLANG_UNIMPLEMENTED_X("Unknown Texture type.");
        break;
    }

    createInfo.subresourceRange.aspectMask = getAspectMaskFromFormat(resourceImpl->m_vkformat);

    createInfo.subresourceRange.baseArrayLayer = desc.subresourceRange.baseArrayLayer;
    createInfo.subresourceRange.baseMipLevel = desc.subresourceRange.mipLevel;
    createInfo.subresourceRange.layerCount = desc.subresourceRange.layerCount == 0
        ? VK_REMAINING_ARRAY_LAYERS
        : desc.subresourceRange.layerCount;
    createInfo.subresourceRange.levelCount = desc.subresourceRange.mipLevelCount == 0
        ? VK_REMAINING_MIP_LEVELS
        : desc.subresourceRange.mipLevelCount;
    switch (desc.type)
    {
    case IResourceView::Type::DepthStencil:
        view->m_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        createInfo.subresourceRange.levelCount = 1;
        break;
    case IResourceView::Type::RenderTarget:
        view->m_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        createInfo.subresourceRange.levelCount = 1;
        break;
    case IResourceView::Type::ShaderResource:
        view->m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        break;
    case IResourceView::Type::UnorderedAccess:
        view->m_layout = VK_IMAGE_LAYOUT_GENERAL;
        break;
    default:
        SLANG_UNIMPLEMENTED_X("Unknown TextureViewDesc type.");
        break;
    }
    m_api.vkCreateImageView(m_device, &createInfo, nullptr, &view->m_view);
    returnComPtr(outView, view);
    return SLANG_OK;
}

Result DeviceImpl::getFormatSupportedResourceStates(Format format, ResourceStateSet* outStates)
{
    // TODO: Add variables to VkDevice to track supported surface presentable formats

    VkFormat vkFormat = VulkanUtil::getVkFormat(format);

    VkFormatProperties supportedProperties = {};
    m_api.vkGetPhysicalDeviceFormatProperties(
        m_api.m_physicalDevice, vkFormat, &supportedProperties);

    HashSet<VkFormat> presentableFormats;
    // TODO: enable this once we have VK_GOOGLE_surfaceless_query.
#if 0
    List<VkSurfaceFormatKHR> surfaceFormats;

    uint32_t surfaceFormatCount = 0;
    m_api.vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_api.m_physicalDevice, VK_NULL_HANDLE, &surfaceFormatCount, nullptr);

    surfaceFormats.setCount(surfaceFormatCount);
    m_api.vkGetPhysicalDeviceSurfaceFormatsKHR(m_api.m_physicalDevice, VK_NULL_HANDLE, &surfaceFormatCount, surfaceFormats.getBuffer());
    for (auto surfaceFormat : surfaceFormats)
    {
        presentableFormats.Add(surfaceFormat.format);
    }
#else
// Until we have a solution to query presentable formats without needing a surface,
// hard code presentable formats that is supported by most drivers.
    presentableFormats.Add(VK_FORMAT_R8G8B8A8_UNORM);
    presentableFormats.Add(VK_FORMAT_B8G8R8A8_UNORM);
    presentableFormats.Add(VK_FORMAT_R8G8B8A8_SRGB);
    presentableFormats.Add(VK_FORMAT_B8G8R8A8_SRGB);
#endif

    ResourceStateSet allowedStates;
    // TODO: Currently only supports VK_IMAGE_TILING_OPTIMAL
    auto imageFeatures = supportedProperties.optimalTilingFeatures;
    auto bufferFeatures = supportedProperties.bufferFeatures;
    // PreInitialized - Only supported for VK_IMAGE_TILING_LINEAR
    // VertexBuffer
    if (bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
        allowedStates.add(ResourceState::VertexBuffer);
    // IndexBuffer - Without extensions, Vulkan only supports two formats for index buffers.
    switch (format)
    {
    case Format::R32_UINT:
    case Format::R16_UINT:
        allowedStates.add(ResourceState::IndexBuffer);
        break;
    default:
        break;
    }
    // ConstantBuffer
    allowedStates.add(ResourceState::ConstantBuffer);
    // StreamOutput - TODO: Requires VK_EXT_transform_feedback
    // ShaderResource
    if (imageFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        allowedStates.add(ResourceState::ShaderResource);
    if (bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)
        allowedStates.add(ResourceState::ShaderResource);
    // UnorderedAccess
    if (imageFeatures &
        (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
        allowedStates.add(ResourceState::UnorderedAccess);
    if (bufferFeatures & (VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
        VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT))
        allowedStates.add(ResourceState::UnorderedAccess);
    // RenderTarget
    if (imageFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        allowedStates.add(ResourceState::RenderTarget);
    // DepthRead, DepthWrite
    if (imageFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        allowedStates.add(ResourceState::DepthRead);
        allowedStates.add(ResourceState::DepthWrite);
    }
    // Present
    if (presentableFormats.Contains(vkFormat))
        allowedStates.add(ResourceState::Present);
    // IndirectArgument
    allowedStates.add(ResourceState::IndirectArgument);
    // CopySource, ResolveSource
    if (imageFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)
    {
        allowedStates.add(ResourceState::CopySource);
        allowedStates.add(ResourceState::ResolveSource);
    }
    // CopyDestination, ResolveDestination
    if (imageFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
    {
        allowedStates.add(ResourceState::CopyDestination);
        allowedStates.add(ResourceState::ResolveDestination);
    }
    // AccelerationStructure
    if (bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR)
    {
        allowedStates.add(ResourceState::AccelerationStructure);
        allowedStates.add(ResourceState::AccelerationStructureBuildInput);
    }

    *outStates = allowedStates;
    return SLANG_OK;
}

Result DeviceImpl::createBufferView(
    IBufferResource* buffer,
    IBufferResource* counterBuffer,
    IResourceView::Desc const& desc,
    IResourceView** outView)
{
    auto resourceImpl = (BufferResourceImpl*)buffer;

    // TODO: These should come from the `ResourceView::Desc`
    auto stride = desc.bufferElementSize;
    if (stride == 0)
    {
        if (desc.format == Format::Unknown)
        {
            stride = 1;
        }
        else
        {
            FormatInfo info;
            gfxGetFormatInfo(desc.format, &info);
            stride = info.blockSizeInBytes;
            assert(info.pixelsPerBlock == 1);
        }
    }
    VkDeviceSize offset = (VkDeviceSize)desc.bufferRange.firstElement * stride;
    VkDeviceSize size = desc.bufferRange.elementCount == 0
        ? (buffer ? resourceImpl->getDesc()->sizeInBytes : 0)
        : (VkDeviceSize)desc.bufferRange.elementCount * stride;

    // There are two different cases we need to think about for buffers.
    //
    // One is when we have a "uniform texel buffer" or "storage texel buffer,"
    // in which case we need to construct a `VkBufferView` to represent the
    // formatting that is applied to the buffer. This case would correspond
    // to a `textureBuffer` or `imageBuffer` in GLSL, and more or less to
    // `Buffer<..>` or `RWBuffer<...>` in HLSL.
    //
    // The other case is a `storage buffer` which is the catch-all for any
    // non-formatted R/W access to a buffer. In GLSL this is a `buffer { ... }`
    // declaration, while in HLSL it covers a bunch of different `RW*Buffer`
    // cases. In these cases we do *not* need a `VkBufferView`, but in
    // order to be compatible with other APIs that require views for any
    // potentially writable access, we will have to create one anyway.
    //
    // We will distinguish the two cases by looking at whether the view
    // is being requested with a format or not.
    //

    switch (desc.type)
    {
    default:
        assert(!"unhandled");
        return SLANG_FAIL;

    case IResourceView::Type::UnorderedAccess:
    case IResourceView::Type::ShaderResource:
        // Is this a formatted view?
        //
        if (desc.format == Format::Unknown)
        {
            // Buffer usage that doesn't involve formatting doesn't
            // require a view in Vulkan.
            RefPtr<PlainBufferResourceViewImpl> viewImpl = new PlainBufferResourceViewImpl(this);
            viewImpl->m_buffer = resourceImpl;
            viewImpl->offset = offset;
            viewImpl->size = size;
            viewImpl->m_desc = desc;

            returnComPtr(outView, viewImpl);
            return SLANG_OK;
        }
        //
        // If the view is formatted, then we need to handle
        // it just like we would for a "sampled" buffer:
        //
        // FALLTHROUGH
        {
            VkBufferViewCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };

            VkBufferView view = VK_NULL_HANDLE;

            if (buffer)
            {
                info.format = VulkanUtil::getVkFormat(desc.format);
                info.buffer = resourceImpl->m_buffer.m_buffer;
                info.offset = offset;
                info.range = size;

                SLANG_VK_RETURN_ON_FAIL(m_api.vkCreateBufferView(m_device, &info, nullptr, &view));
            }

            RefPtr<TexelBufferResourceViewImpl> viewImpl = new TexelBufferResourceViewImpl(this);
            viewImpl->m_buffer = resourceImpl;
            viewImpl->m_view = view;
            viewImpl->m_desc = desc;

            returnComPtr(outView, viewImpl);
            return SLANG_OK;
        }
        break;
    }
}

Result DeviceImpl::createInputLayout(IInputLayout::Desc const& desc, IInputLayout** outLayout)
{
    RefPtr<InputLayoutImpl> layout(new InputLayoutImpl);

    List<VkVertexInputAttributeDescription>& dstAttributes = layout->m_attributeDescs;
    List<VkVertexInputBindingDescription>& dstStreams = layout->m_streamDescs;

    auto elements = desc.inputElements;
    Int numElements = desc.inputElementCount;

    auto srcVertexStreams = desc.vertexStreams;
    Int vertexStreamCount = desc.vertexStreamCount;

    dstAttributes.setCount(numElements);
    dstStreams.setCount(vertexStreamCount);

    for (Int i = 0; i < vertexStreamCount; i++)
    {
        auto& dstStream = dstStreams[i];
        auto& srcStream = srcVertexStreams[i];
        dstStream.stride = (uint32_t)srcStream.stride;
        dstStream.binding = (uint32_t)i;
        dstStream.inputRate = (srcStream.slotClass == InputSlotClass::PerInstance)
            ? VK_VERTEX_INPUT_RATE_INSTANCE
            : VK_VERTEX_INPUT_RATE_VERTEX;
    }

    for (Int i = 0; i < numElements; ++i)
    {
        const InputElementDesc& srcDesc = elements[i];
        auto streamIndex = srcDesc.bufferSlotIndex;

        VkVertexInputAttributeDescription& dstDesc = dstAttributes[i];

        dstDesc.location = uint32_t(i);
        dstDesc.binding = (uint32_t)streamIndex;
        dstDesc.format = VulkanUtil::getVkFormat(srcDesc.format);
        if (dstDesc.format == VK_FORMAT_UNDEFINED)
        {
            return SLANG_FAIL;
        }

        dstDesc.offset = uint32_t(srcDesc.offset);
    }

    // Work out the overall size
    returnComPtr(outLayout, layout);
    return SLANG_OK;
}

Result DeviceImpl::createProgram(
    const IShaderProgram::Desc& desc, IShaderProgram** outProgram, ISlangBlob** outDiagnosticBlob)
{
    RefPtr<ShaderProgramImpl> shaderProgram = new ShaderProgramImpl(this);
    shaderProgram->init(desc);

    m_deviceObjectsWithPotentialBackReferences.add(shaderProgram);

    RootShaderObjectLayout::create(
        this,
        shaderProgram->linkedProgram,
        shaderProgram->linkedProgram->getLayout(),
        shaderProgram->m_rootObjectLayout.writeRef());

    returnComPtr(outProgram, shaderProgram);
    return SLANG_OK;
}

Result DeviceImpl::createShaderObjectLayout(
    slang::TypeLayoutReflection* typeLayout, ShaderObjectLayoutBase** outLayout)
{
    RefPtr<ShaderObjectLayoutImpl> layout;
    SLANG_RETURN_ON_FAIL(
        ShaderObjectLayoutImpl::createForElementType(this, typeLayout, layout.writeRef()));
    returnRefPtrMove(outLayout, layout);
    return SLANG_OK;
}

Result DeviceImpl::createShaderObject(ShaderObjectLayoutBase* layout, IShaderObject** outObject)
{
    RefPtr<ShaderObjectImpl> shaderObject;
    SLANG_RETURN_ON_FAIL(ShaderObjectImpl::create(
        this, static_cast<ShaderObjectLayoutImpl*>(layout), shaderObject.writeRef()));
    returnComPtr(outObject, shaderObject);
    return SLANG_OK;
}

Result DeviceImpl::createMutableShaderObject(
    ShaderObjectLayoutBase* layout, IShaderObject** outObject)
{
    auto layoutImpl = static_cast<ShaderObjectLayoutImpl*>(layout);

    RefPtr<MutableShaderObjectImpl> result = new MutableShaderObjectImpl();
    SLANG_RETURN_ON_FAIL(result->init(this, layoutImpl));
    returnComPtr(outObject, result);

    return SLANG_OK;
}

Result DeviceImpl::createMutableRootShaderObject(IShaderProgram* program, IShaderObject** outObject)
{
    RefPtr<MutableRootShaderObject> result =
        new MutableRootShaderObject(this, static_cast<ShaderProgramBase*>(program));
    returnComPtr(outObject, result);
    return SLANG_OK;
}

Result DeviceImpl::createShaderTable(const IShaderTable::Desc& desc, IShaderTable** outShaderTable)
{
    RefPtr<ShaderTableImpl> result = new ShaderTableImpl();
    result->m_device = this;
    result->init(desc);
    returnComPtr(outShaderTable, result);
    return SLANG_OK;
}

Result DeviceImpl::createGraphicsPipelineState(
    const GraphicsPipelineStateDesc& inDesc, IPipelineState** outState)
{
    GraphicsPipelineStateDesc desc = inDesc;
    RefPtr<PipelineStateImpl> pipelineStateImpl = new PipelineStateImpl(this);
    pipelineStateImpl->init(desc);
    pipelineStateImpl->establishStrongDeviceReference();
    m_deviceObjectsWithPotentialBackReferences.add(pipelineStateImpl);
    returnComPtr(outState, pipelineStateImpl);

    return SLANG_OK;
}

Result DeviceImpl::createComputePipelineState(
    const ComputePipelineStateDesc& inDesc, IPipelineState** outState)
{
    ComputePipelineStateDesc desc = inDesc;
    RefPtr<PipelineStateImpl> pipelineStateImpl = new PipelineStateImpl(this);
    pipelineStateImpl->init(desc);
    m_deviceObjectsWithPotentialBackReferences.add(pipelineStateImpl);
    pipelineStateImpl->establishStrongDeviceReference();
    returnComPtr(outState, pipelineStateImpl);
    return SLANG_OK;
}

Result DeviceImpl::createRayTracingPipelineState(
    const RayTracingPipelineStateDesc& desc, IPipelineState** outState)
{
    RefPtr<RayTracingPipelineStateImpl> pipelineStateImpl = new RayTracingPipelineStateImpl(this);
    pipelineStateImpl->init(desc);
    m_deviceObjectsWithPotentialBackReferences.add(pipelineStateImpl);
    pipelineStateImpl->establishStrongDeviceReference();
    returnComPtr(outState, pipelineStateImpl);
    return SLANG_OK;
}

Result DeviceImpl::createQueryPool(const IQueryPool::Desc& desc, IQueryPool** outPool)
{
    RefPtr<QueryPoolImpl> result = new QueryPoolImpl();
    SLANG_RETURN_ON_FAIL(result->init(desc, this));
    returnComPtr(outPool, result);
    return SLANG_OK;
}

Result DeviceImpl::createFence(const IFence::Desc& desc, IFence** outFence)
{
    RefPtr<FenceImpl> fence = new FenceImpl(this);
    SLANG_RETURN_ON_FAIL(fence->init(desc));
    returnComPtr(outFence, fence);
    return SLANG_OK;
}

Result DeviceImpl::waitForFences(
    GfxCount fenceCount, IFence** fences, uint64_t* fenceValues, bool waitForAll, uint64_t timeout)
{
    ShortList<VkSemaphore> semaphores;
    for (GfxIndex i = 0; i < fenceCount; ++i)
    {
        auto fenceImpl = static_cast<FenceImpl*>(fences[i]);
        semaphores.add(fenceImpl->m_semaphore);
    }
    VkSemaphoreWaitInfo waitInfo;
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.pNext = NULL;
    waitInfo.flags = 0;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = semaphores.getArrayView().getBuffer();
    waitInfo.pValues = fenceValues;
    auto result = m_api.vkWaitSemaphores(m_api.m_device, &waitInfo, timeout);
    if (result == VK_TIMEOUT)
        return SLANG_E_TIME_OUT;
    return result == VK_SUCCESS ? SLANG_OK : SLANG_FAIL;
}

} // namespace vk
} // namespace gfx