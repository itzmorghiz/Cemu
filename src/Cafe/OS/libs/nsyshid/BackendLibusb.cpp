#include "BackendLibusb.h"

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

namespace nsyshid::backend::libusb
{
	BackendLibusb::BackendLibusb()
		: m_ctx(nullptr),
		  m_initReturnCode(0),
		  m_callbackRegistered(false),
		  m_hotplugCallbackHandle(0),
		  m_hotplugThreadStop(false)
	{
		m_initReturnCode = libusb_init(&m_ctx);
		if (m_initReturnCode < 0)
		{
			m_ctx = nullptr;
			cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb: failed to initialize libusb, return code: {}",
						m_initReturnCode);
			return;
		}

		if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
		{
			int ret = libusb_hotplug_register_callback(m_ctx,
													   (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
																			  LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
													   (libusb_hotplug_flag)0,
													   LIBUSB_HOTPLUG_MATCH_ANY,
													   LIBUSB_HOTPLUG_MATCH_ANY,
													   LIBUSB_HOTPLUG_MATCH_ANY,
													   HotplugCallback,
													   this,
													   &m_hotplugCallbackHandle);
			if (ret != LIBUSB_SUCCESS)
			{
				cemuLog_logDebug(LogType::Force,
							"nsyshid::BackendLibusb: failed to register hotplug callback with return code {}",
							ret);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb: registered hotplug callback");
				m_callbackRegistered = true;
				m_hotplugThread = std::thread([this] {
					while (!m_hotplugThreadStop)
					{
						timeval timeout{
							.tv_sec = 1,
							.tv_usec = 0,
						};
						int ret = libusb_handle_events_timeout_completed(m_ctx, &timeout, nullptr);
						if (ret != 0)
						{
							cemuLog_logDebug(LogType::Force,
										"nsyshid::BackendLibusb: hotplug thread: error handling events: {}",
										ret);
							std::this_thread::sleep_for(std::chrono::milliseconds(1000));
						}
					}
				});
			}
		}
		else
		{
			cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb: hotplug not supported by this version of libusb");
		}
	}

	bool BackendLibusb::IsInitialisedOk()
	{
		return m_initReturnCode == 0;
	}

	void BackendLibusb::AttachVisibleDevices()
	{
		// 1. Enumerazione standard libusb (USB)
		libusb_device** devices;
		ssize_t deviceCount = libusb_get_device_list(m_ctx, &devices);
		if (deviceCount >= 0)
		{
			libusb_device* dev;
			for (int i = 0; (dev = devices[i]) != nullptr; i++)
			{
				auto device = CheckAndCreateDevice(dev);
				if (device != nullptr)
				{
					if (IsDeviceWhitelisted(device->m_vendorId, device->m_productId))
					{
						if (!AttachDevice(device))
						{
							cemuLog_log(LogType::Force,
										"nsyshid::BackendLibusb: failed to attach device: {:04x}:{:04x}",
										device->m_vendorId,
										device->m_productId);
						}
					}
					else
					{
						cemuLog_log(LogType::Force,
									"nsyshid::BackendLibusb: device not on whitelist: {:04x}:{:04x}",
									device->m_vendorId,
									device->m_productId);
					}
				}
			}
			libusb_free_device_list(devices, 1);
		}

#if defined(_WIN32)
		// 2. Enumerazione dispositivi Bluetooth HID tramite SetupAPI su Windows
		GUID hidGuid;
		HidD_GetHidGuid(&hidGuid);

		HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
		if (deviceInfoSet != INVALID_HANDLE_VALUE)
		{
			SP_DEVICE_INTERFACE_DATA interfaceData;
			interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

			for (DWORD index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &hidGuid, index, &interfaceData); index++)
			{
				DWORD requiredSize = 0;
				SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &interfaceData, nullptr, 0, &requiredSize, nullptr);
				if (requiredSize == 0) continue;

				std::vector<char> detailBuffer(requiredSize);
				auto* detData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(detailBuffer.data());
				detData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

				if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &interfaceData, detData, requiredSize, nullptr, nullptr))
				{
					HANDLE hFile = CreateFile(detData->DevicePath, GENERIC_READ | GENERIC_WRITE,
											  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
											  FILE_FLAG_OVERLAPPED, nullptr);

					if (hFile != INVALID_HANDLE_VALUE)
					{
						HIDD_ATTRIBUTES attrib = { sizeof(HIDD_ATTRIBUTES) };
						if (HidD_GetAttributes(hFile, &attrib))
						{
							if (IsDeviceWhitelisted(attrib.VendorID, attrib.ProductID))
							{
								auto winDevice = std::make_shared<DeviceLibusb>(attrib.VendorID, attrib.ProductID, std::string(detData->DevicePath));
								if (!AttachDevice(winDevice))
								{
									cemuLog_log(LogType::Force,
												"nsyshid::BackendLibusb: failed to attach Windows Bluetooth HID device: {:04x}:{:04x}",
												attrib.VendorID,
												attrib.ProductID);
								}
								else
								{
									cemuLog_logDebug(LogType::Force,
												"nsyshid::BackendLibusb: successfully attached Windows Bluetooth HID device: {:04x}:{:04x}",
												attrib.VendorID,
												attrib.ProductID);
								}
							}
						}
						CloseHandle(hFile);
					}
				}
			}
			SetupDiDestroyDeviceInfoList(deviceInfoSet);
		}
#endif
	}

	int BackendLibusb::HotplugCallback(libusb_context* ctx,
									   libusb_device* dev,
									   libusb_hotplug_event event,
									   void* user_data)
	{
		if (user_data)
		{
			BackendLibusb* backend = static_cast<BackendLibusb*>(user_data);
			return backend->OnHotplug(dev, event);
		}
		return 0;
	}

	int BackendLibusb::OnHotplug(libusb_device* dev, libusb_hotplug_event event)
	{
		struct libusb_device_descriptor desc;
		int ret = libusb_get_device_descriptor(dev, &desc);
		if (ret < 0)
		{
			cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb::OnHotplug(): failed to get device descriptor");
			return 0;
		}

		switch (event)
		{
		case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb::OnHotplug(): device arrived: {:04x}:{:04x}",
						desc.idVendor,
						desc.idProduct);
			auto device = CheckAndCreateDevice(dev);
			if (device != nullptr)
			{
				if (IsDeviceWhitelisted(device->m_vendorId, device->m_productId))
				{
					if (!AttachDevice(device))
					{
						cemuLog_log(LogType::Force,
									"nsyshid::BackendLibusb::OnHotplug(): failed to attach device: {:04x}:{:04x}",
									device->m_vendorId,
									device->m_productId);
					}
				}
				else
				{
					cemuLog_log(LogType::Force,
								"nsyshid::BackendLibusb::OnHotplug(): device not on whitelist: {:04x}:{:04x}",
								device->m_vendorId,
								device->m_productId);
				}
			}
		}
		break;
		case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
		{
			cemuLog_logDebug(LogType::Force, "nsyshid::BackendLibusb::OnHotplug(): device left: {:04x}:{:04x}",
						desc.idVendor,
						desc.idProduct);
			auto device = FindLibusbDevice(dev);
			if (device != nullptr)
			{
				DetachDevice(device);
			}
		}
		break;
		}

		return 0;
	}

	BackendLibusb::~BackendLibusb()
	{
		if (m_callbackRegistered)
		{
			m_hotplugThreadStop = true;
			libusb_hotplug_deregister_callback(m_ctx, m_hotplugCallbackHandle);
			m_hotplugThread.join();
		}
		DetachAllDevices();
		if (m_ctx)
		{
			libusb_exit(m_ctx);
			m_ctx = nullptr;
		}
	}

	std::shared_ptr<Device> BackendLibusb::FindLibusbDevice(libusb_device* dev)
	{
		libusb_device_descriptor desc;
		int ret = libusb_get_device_descriptor(dev, &desc);
		if (ret < 0)
		{
			cemuLog_logDebug(LogType::Force,
						"nsyshid::BackendLibusb::FindLibusbDevice(): failed to get device descriptor");
			return nullptr;
		}
		uint8 busNumber = libusb_get_bus_number(dev);
		uint8 deviceAddress = libusb_get_device_address(dev);
		auto device = FindDevice([desc, busNumber, deviceAddress](const std::shared_ptr<Device>& d) -> bool {
			auto device = std::dynamic_pointer_cast<DeviceLibusb>(d);
			if (device != nullptr &&
				desc.idVendor == device->m_vendorId &&
				desc.idProduct == device->m_productId &&
				busNumber == device->m_libusbBusNumber &&
				deviceAddress == device->m_libusbDeviceAddress)
			{
				return true;
			}
			return false;
		});

		if (device != nullptr)
		{
			return device;
		}
		return nullptr;
	}

	std::pair<int, ConfigDescriptor> MakeConfigDescriptor(libusb_device* device, uint8 config_num)
	{
		libusb_config_descriptor* descriptor = nullptr;
		const int ret = libusb_get_config_descriptor(device, config_num, &descriptor);
		if (ret == LIBUSB_SUCCESS)
			return {ret, ConfigDescriptor{descriptor, libusb_free_config_descriptor}};

		return {ret, ConfigDescriptor{nullptr, [](auto) {
									  }}};
	}

	std::shared_ptr<Device> BackendLibusb::CheckAndCreateDevice(libusb_device* dev)
	{
		struct libusb_device_descriptor desc;
		int ret = libusb_get_device_descriptor(dev, &desc);
		if (ret < 0)
		{
			cemuLog_log(LogType::Force,
						"nsyshid::BackendLibusb::CheckAndCreateDevice(): failed to get device descriptor; return code: %i",
						ret);
			return nullptr;
		}
		std::vector<ConfigDescriptor> config_descriptors{};
		for (uint8 i = 0; i < desc.bNumConfigurations; ++i)
		{
			auto [ret, config_descriptor] = MakeConfigDescriptor(dev, i);
			if (ret != LIBUSB_SUCCESS || !config_descriptor)
			{
				cemuLog_log(LogType::Force, "Failed to make config descriptor {} for {:04x}:{:04x}: {}",
							i, desc.idVendor, desc.idProduct, libusb_error_name(ret));
			}
			else
			{
				config_descriptors.emplace_back(std::move(config_descriptor));
			}
		}
		if (desc.idVendor == 0x0e6f && desc.idProduct == 0x0241)
		{
			cemuLog_logDebug(LogType::Force,
						"nsyshid::BackendLibusb::CheckAndCreateDevice(): lego dimensions portal detected");
		}
		auto device = std::make_shared<DeviceLibusb>(m_ctx,
													 desc.idVendor,
													 desc.idProduct,
													 0,
													 2,
													 0,
													 libusb_get_bus_number(dev),
													 libusb_get_device_address(dev),
													 std::move(config_descriptors));
		if (!FindDefaultDeviceEndpoints(dev,
										device->m_libusbHasEndpointIn,
										device->m_libusbEndpointIn,
										device->m_maxPacketSizeRX,
										device->m_libusbHasEndpointOut,
										device->m_libusbEndpointOut,
										device->m_maxPacketSizeTX))
		{
			cemuLog_log(LogType::Force,
						"nsyshid::BackendLibusb::CheckAndCreateDevice(): failed to find default endpoints for device: {:04x}:{:04x}",
						device->m_vendorId,
						device->m_productId);
			return nullptr;
		}
		return device;
	}

	bool BackendLibusb::FindDefaultDeviceEndpoints(libusb_device* dev, bool& endpointInFound, uint8& endpointIn,
												   uint16& endpointInMaxPacketSize, bool& endpointOutFound,
												   uint8& endpointOut, uint16& endpointOutMaxPacketSize)
	{
		endpointInFound = false;
		endpointIn = 0;
		endpointInMaxPacketSize = 0;
		endpointOutFound = false;
		endpointOut = 0;
		endpointOutMaxPacketSize = 0;

		struct libusb_config_descriptor* conf = nullptr;
		int ret = libusb_get_active_config_descriptor(dev, &conf);

		if (ret == 0)
		{
			for (uint8 interfaceIndex = 0; interfaceIndex < conf->bNumInterfaces; interfaceIndex++)
			{
				const struct libusb_interface& interface = conf->interface[interfaceIndex];
				for (int altsettingIndex = 0; altsettingIndex < interface.num_altsetting; altsettingIndex++)
				{
					const struct libusb_interface_descriptor& altsetting = interface.altsetting[altsettingIndex];
					for (uint8 endpointIndex = 0; endpointIndex < altsetting.bNumEndpoints; endpointIndex++)
					{
						const struct libusb_endpoint_descriptor& endpoint = altsetting.endpoint[endpointIndex];
						if ((endpoint.bEndpointAddress & (1 << 7)) != 0)
						{
							if (!endpointInFound)
							{
								endpointInFound = true;
								endpointIn = endpoint.bEndpointAddress;
								endpointInMaxPacketSize = endpoint.wMaxPacketSize;
							}
						}
						else
						{
							if (!endpointOutFound)
							{
								endpointOutFound = true;
								endpointOut = endpoint.bEndpointAddress;
								endpointOutMaxPacketSize = endpoint.wMaxPacketSize;
							}
						}
					}
				}
			}
			libusb_free_config_descriptor(conf);
			return true;
		}
		return false;
	}

	DeviceLibusb::DeviceLibusb(libusb_context* ctx,
							   uint16 vendorId,
							   uint16 productId,
							   uint8 interfaceIndex,
							   uint8 interfaceSubClass,
							   uint8 protocol,
							   uint8 libusbBusNumber,
							   uint8 libusbDeviceAddress,
							   std::vector<ConfigDescriptor> configs)
		: Device(vendorId,
				 productId,
				 interfaceIndex,
				 interfaceSubClass,
				 protocol),
		  m_ctx(ctx),
		  m_libusbHandle(nullptr),
		  m_handleInUseCounter(-1),
		  m_libusbBusNumber(libusbBusNumber),
		  m_libusbDeviceAddress(libusbDeviceAddress),
		  m_libusbHasEndpointIn(false),
		  m_libusbEndpointIn(0),
		  m_libusbHasEndpointOut(false),
		  m_libusbEndpointOut(0)
#if defined(_WIN32)
		  , m_isWindowsHidBluetooth(false),
		    m_winFileHandle(INVALID_HANDLE_VALUE)
#endif
	{
		m_config_descriptors = std::move(configs);
	}

#if defined(_WIN32)
	DeviceLibusb::DeviceLibusb(uint16 vendorId,
							   uint16 productId,
							   std::string winDevicePath)
		: Device(vendorId,
				 productId,
				 0,
				 0,
				 0),
		  m_ctx(nullptr),
		  m_libusbHandle(nullptr),
		  m_handleInUseCounter(-1),
		  m_libusbBusNumber(0),
		  m_libusbDeviceAddress(0),
		  m_libusbHasEndpointIn(true),
		  m_libusbEndpointIn(0),
		  m_libusbHasEndpointOut(true),
		  m_libusbEndpointOut(0),
		  m_isWindowsHidBluetooth(true),
		  m_winDevicePath(winDevicePath),
		  m_winFileHandle(INVALID_HANDLE_VALUE)
	{
		m_maxPacketSizeRX = 64;
		m_maxPacketSizeTX = 64;
	}
#endif

	DeviceLibusb::~DeviceLibusb()
	{
		CloseDevice();
	}

	bool DeviceLibusb::Open()
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			std::unique_lock<std::mutex> lock(m_handleMutex);
			if (IsOpened()) return true;

			m_winFileHandle = CreateFileA(m_winDevicePath.c_str(), 
										  GENERIC_READ | GENERIC_WRITE,
										  FILE_SHARE_READ | FILE_SHARE_WRITE, 
										  nullptr, 
										  OPEN_EXISTING, 
										  FILE_FLAG_OVERLAPPED, 
										  nullptr);

			if (m_winFileHandle == INVALID_HANDLE_VALUE)
			{
				cemuLog_log(LogType::Force, "nsyshid::DeviceLibusb::open(): failed to open Windows Bluetooth HID device");
				return false;
			}
			m_handleInUseCounter = 0;
			return true;
		}
#endif

		std::unique_lock<std::mutex> lock(m_handleMutex);
		if (IsOpened())
		{
			return true;
		}
		while (m_handleInUseCounter != -1)
		{
			m_handleInUseCounterDecremented.wait(lock);
		}

		libusb_device** devices;
		ssize_t deviceCount = libusb_get_device_list(m_ctx, &devices);
		if (deviceCount < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid::DeviceLibusb::open(): failed to get usb devices");
			return false;
		}
		libusb_device* dev;
		libusb_device* found = nullptr;
		for (int i = 0; (dev = devices[i]) != nullptr; i++)
		{
			struct libusb_device_descriptor desc;
			int ret = libusb_get_device_descriptor(dev, &desc);
			if (ret < 0)
			{
				libusb_free_device_list(devices, 1);
				return false;
			}
			if (desc.idVendor == this->m_vendorId &&
				desc.idProduct == this->m_productId &&
				libusb_get_bus_number(dev) == this->m_libusbBusNumber &&
				libusb_get_device_address(dev) == this->m_libusbDeviceAddress)
			{
				found = dev;
				break;
			}
		}

		if (found != nullptr)
		{
			int ret = libusb_open(found, &(this->m_libusbHandle));
			if (ret < 0)
			{
				this->m_libusbHandle = nullptr;
				libusb_free_device_list(devices, 1);
				return false;
			}
			this->m_handleInUseCounter = 0;
			ClaimAllInterfaces(0);
		}

		libusb_free_device_list(devices, 1);
		return found != nullptr;
	}

	void DeviceLibusb::Close()
	{
		CloseDevice();
	}

	void DeviceLibusb::CloseDevice()
	{
		std::unique_lock<std::mutex> lock(m_handleMutex);
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			if (m_winFileHandle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_winFileHandle);
				m_winFileHandle = INVALID_HANDLE_VALUE;
			}
			m_handleInUseCounter = -1;
			return;
		}
#endif

		if (IsOpened())
		{
			auto handle = m_libusbHandle;
			m_libusbHandle = nullptr;
			while (m_handleInUseCounter > 0)
			{
				m_handleInUseCounterDecremented.wait(lock);
			}
			ReleaseAllInterfacesForCurrentConfig();
			libusb_close(handle);
			m_handleInUseCounter = -1;
			m_handleInUseCounterDecremented.notify_all();
		}
	}

	bool DeviceLibusb::IsOpened()
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			return m_winFileHandle != INVALID_HANDLE_VALUE && m_handleInUseCounter >= 0;
		}
#endif
		return m_libusbHandle != nullptr && m_handleInUseCounter >= 0;
	}

	Device::ReadResult DeviceLibusb::Read(ReadMessage* message)
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			DWORD bytesRead = 0;
			OVERLAPPED overlapped = {0};
			overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

			if (ReadFile(m_winFileHandle, message->data, message->length, &bytesRead, &overlapped))
			{
				message->bytesRead = bytesRead;
				CloseHandle(overlapped.hEvent);
				return ReadResult::Success;
			}
			
			if (GetLastError() == ERROR_IO_PENDING)
			{
				if (GetOverlappedResult(m_winFileHandle, &overlapped, &bytesRead, TRUE))
				{
					message->bytesRead = bytesRead;
					CloseHandle(overlapped.hEvent);
					return ReadResult::Success;
				}
			}
			CloseHandle(overlapped.hEvent);
			return ReadResult::Error;
		}
#endif

		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid())
		{
			return ReadResult::Error;
		}

		for (int i = 0; i < m_config_descriptors.size(); i++)
		{
			ClaimAllInterfaces(i);
		}

		const unsigned int timeout = 50;
		int actualLength = 0;
		int ret = 0;
		do
		{
			ret = libusb_interrupt_transfer(handleLock->GetHandle(),
											this->m_libusbEndpointIn,
											message->data,
											message->length,
											&actualLength,
											timeout);
		}
		while (ret == LIBUSB_ERROR_TIMEOUT && actualLength == 0 && IsOpened());

		if (ret == 0 || ret == LIBUSB_ERROR_TIMEOUT)
		{
			message->bytesRead = actualLength;
			return ReadResult::Success;
		}
		return ReadResult::Error;
	}

	Device::WriteResult DeviceLibusb::Write(WriteMessage* message)
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			DWORD bytesWritten = 0;
			if (WriteFile(m_winFileHandle, message->data, message->length, &bytesWritten, nullptr))
			{
				message->bytesWritten = bytesWritten;
				return WriteResult::Success;
			}
			return WriteResult::Error;
		}
#endif

		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid())
		{
			return WriteResult::Error;
		}

		for (int i = 0; i < m_config_descriptors.size(); i++)
		{
			ClaimAllInterfaces(i);
		}

		message->bytesWritten = 0;
		int actualLength = 0;
		int ret = libusb_interrupt_transfer(handleLock->GetHandle(),
											this->m_libusbEndpointOut,
											message->data,
											message->length,
											&actualLength,
											0);

		if (ret == 0)
		{
			message->bytesWritten = actualLength;
			return WriteResult::Success;
		}
		return WriteResult::Error;
	}

	bool DeviceLibusb::GetDescriptor(uint8 descType,
									 uint8 descIndex,
									 uint16 lang,
									 uint8* output,
									 uint32 outputMaxLength)
	{
		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid())
		{
			return false;
		}

		if (descType == 0x02)
		{
			struct libusb_config_descriptor* conf = nullptr;
			libusb_device* dev = libusb_get_device(handleLock->GetHandle());
			int ret = libusb_get_active_config_descriptor(dev, &conf);
			if (ret == 0)
			{
				std::vector<uint8> configurationDescriptor(conf->wTotalLength);
				uint8* currentWritePtr = &configurationDescriptor[0];

				cemu_assert_debug(conf->bLength == LIBUSB_DT_CONFIG_SIZE);
				*(uint8*)(currentWritePtr + 0) = conf->bLength;
				*(uint8*)(currentWritePtr + 1) = conf->bDescriptorType;
				*(uint16be*)(currentWritePtr + 2) = conf->wTotalLength;
				*(uint8*)(currentWritePtr + 4) = conf->bNumInterfaces;
				*(uint8*)(currentWritePtr + 5) = conf->bConfigurationValue;
				*(uint8*)(currentWritePtr + 6) = conf->iConfiguration;
				*(uint8*)(currentWritePtr + 7) = conf->bmAttributes;
				*(uint8*)(currentWritePtr + 8) = conf->MaxPower;
				currentWritePtr = currentWritePtr + conf->bLength;

				for (uint8_t interfaceIndex = 0; interfaceIndex < conf->bNumInterfaces; interfaceIndex++)
				{
					const struct libusb_interface& interface = conf->interface[interfaceIndex];
					for (int altsettingIndex = 0; altsettingIndex < interface.num_altsetting; altsettingIndex++)
					{
						const struct libusb_interface_descriptor& altsetting = interface.altsetting[altsettingIndex];
						cemu_assert_debug(altsetting.bLength == LIBUSB_DT_INTERFACE_SIZE);
						*(uint8*)(currentWritePtr + 0) = altsetting.bLength;
						*(uint8*)(currentWritePtr + 1) = altsetting.bDescriptorType;
						*(uint8*)(currentWritePtr + 2) = altsetting.bInterfaceNumber;
						*(uint8*)(currentWritePtr + 3) = altsetting.bAlternateSetting;
						*(uint8*)(currentWritePtr + 4) = altsetting.bNumEndpoints;
						*(uint8*)(currentWritePtr + 5) = altsetting.bInterfaceClass;
						*(uint8*)(currentWritePtr + 6) = altsetting.bInterfaceSubClass;
						*(uint8*)(currentWritePtr + 7) = altsetting.bInterfaceProtocol;
						*(uint8*)(currentWritePtr + 8) = altsetting.iInterface;
						currentWritePtr = currentWritePtr + altsetting.bLength;

						if (altsetting.extra_length > 0)
						{
							const unsigned char* extraReadPointer = altsetting.extra;
							while (extraReadPointer - altsetting.extra < altsetting.extra_length)
							{
								uint8 bLength = *(uint8*)(extraReadPointer + 0);
								if (bLength == 0) break;
								if (extraReadPointer + bLength - altsetting.extra > altsetting.extra_length) break;
								uint8 bDescriptorType = *(uint8*)(extraReadPointer + 1);
								if (bDescriptorType == LIBUSB_DT_HID && bLength == 9)
								{
									*(uint8*)(currentWritePtr + 0) = *(uint8*)(extraReadPointer + 0);
									*(uint8*)(currentWritePtr + 1) = *(uint8*)(extraReadPointer + 1);
									*(uint16be*)(currentWritePtr + 2) = *(uint16*)(extraReadPointer + 2);
									*(uint8*)(currentWritePtr + 4) = *(uint8*)(extraReadPointer + 4);
									*(uint8*)(currentWritePtr + 5) = *(uint8*)(extraReadPointer + 5);
									*(uint8*)(currentWritePtr + 6) = *(uint8*)(extraReadPointer + 6);
									*(uint16be*)(currentWritePtr + 7) = *(uint16*)(extraReadPointer + 7);
									currentWritePtr += bLength;
								}
								extraReadPointer += bLength;
							}
						}
						for (int endpointIndex = 0; endpointIndex < altsetting.bNumEndpoints; endpointIndex++)
						{
							const struct libusb_endpoint_descriptor& endpoint = altsetting.endpoint[endpointIndex];
							cemu_assert_debug(endpoint.bLength == LIBUSB_DT_ENDPOINT_SIZE ||
											  endpoint.bLength == LIBUSB_DT_ENDPOINT_AUDIO_SIZE);
							*(uint8*)(currentWritePtr + 0) = endpoint.bLength;
							*(uint8*)(currentWritePtr + 1) = endpoint.bDescriptorType;
							*(uint8*)(currentWritePtr + 2) = endpoint.bEndpointAddress;
							*(uint8*)(currentWritePtr + 3) = endpoint.bmAttributes;
							*(uint16be*)(currentWritePtr + 4) = endpoint.wMaxPacketSize;
							*(uint8*)(currentWritePtr + 6) = endpoint.bInterval;
							if (endpoint.bLength == LIBUSB_DT_ENDPOINT_AUDIO_SIZE)
							{
								*(uint8*)(currentWritePtr + 7) = endpoint.bRefresh;
								*(uint8*)(currentWritePtr + 8) = endpoint.bSynchAddress;
							}
							currentWritePtr += endpoint.bLength;
						}
					}
				}
				uint32 bytesWritten = currentWritePtr - &configurationDescriptor[0];
				libusb_free_config_descriptor(conf);
				cemu_assert_debug(bytesWritten <= conf->wTotalLength);
				memcpy(output, &configurationDescriptor[0],
					   std::min<uint32>(outputMaxLength, bytesWritten));
				return true;
			}
		}
		else
		{
			uint16 wValue = uint16(descType) << 8 | uint16(descIndex);
			int ret = libusb_control_transfer(handleLock->GetHandle(),
											  LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_ENDPOINT_IN,
											  LIBUSB_REQUEST_GET_DESCRIPTOR,
											  wValue,
											  lang,
											  output,
											  outputMaxLength,
											  0);
			if (ret != outputMaxLength)
			{
				return false;
			}
		}
		return true;
	}

	bool DeviceLibusb::SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration)
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth) return true;
#endif
		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid()) return false;

		uint16 wValue = uint16(duration) << 8 | uint16(reportId);
		int ret = libusb_control_transfer(handleLock->GetHandle(),
										  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
										  HID_CLASS_SET_IDLE,
										  wValue,
										  ifIndex,
										  nullptr,
										  0,
										  0);
		return ret == 0;
	}

	template<typename Configs, typename Function>
	static int DoForEachInterface(const Configs& configs, uint8 config_num, Function action)
	{
		int ret = LIBUSB_ERROR_NOT_FOUND;
		if (configs.size() <= config_num || !configs[config_num])
			return ret;
		for (uint8 i = 0; i < configs[config_num]->bNumInterfaces; ++i)
		{
			ret = action(i);
			if (ret < LIBUSB_SUCCESS)
				break;
		}
		return ret;
	}

	int DeviceLibusb::ClaimAllInterfaces(uint8 config_num)
	{
		const int ret = DoForEachInterface(m_config_descriptors, config_num, [this](uint8 i) {
			if (libusb_kernel_driver_active(this->m_libusbHandle, i))
			{
				const int ret2 = libusb_detach_kernel_driver(this->m_libusbHandle, i);
				if (ret2 < LIBUSB_SUCCESS && ret2 != LIBUSB_ERROR_NOT_FOUND &&
					ret2 != LIBUSB_ERROR_NOT_SUPPORTED)
				{
					return ret2;
				}
			}
			return libusb_claim_interface(this->m_libusbHandle, i);
		});
		return ret;
	}

	int DeviceLibusb::ReleaseAllInterfaces(uint8 config_num)
	{
		const int ret = DoForEachInterface(m_config_descriptors, config_num, [this](uint8 i) {
			return libusb_release_interface(AquireHandleLock()->GetHandle(), i);
		});
		return ret;
	}

	int DeviceLibusb::ReleaseAllInterfacesForCurrentConfig()
	{
		int config_num;
		const int get_config_ret = libusb_get_configuration(AquireHandleLock()->GetHandle(), &config_num);
		if (get_config_ret < LIBUSB_SUCCESS)
			return get_config_ret;
		return ReleaseAllInterfaces(config_num);
	}

	bool DeviceLibusb::SetProtocol(uint8 ifIndex, uint8 protocol)
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth) return true;
#endif
		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid()) return false;

		int ret = libusb_control_transfer(handleLock->GetHandle(),
										  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
										  HID_CLASS_SET_PROTOCOL,
										  protocol,
										  ifIndex,
										  nullptr,
										  0,
										  0);
		return ret == 0;
	}

	bool DeviceLibusb::SetReport(ReportMessage* message)
	{
#if defined(_WIN32)
		if (m_isWindowsHidBluetooth)
		{
			if (m_winFileHandle == INVALID_HANDLE_VALUE) return false;
			return HidD_SetOutputReport(m_winFileHandle, message->data, message->length) == TRUE;
		}
#endif

		auto handleLock = AquireHandleLock();
		if (!handleLock->IsValid())
		{
			return false;
		}

		uint16 wValue = uint16(message->reportType) << 8 | uint16(message->reportId);
		int ret = libusb_control_transfer(handleLock->GetHandle(),
										  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
										  HID_CLASS_SET_REPORT,
										  wValue,
										  m_interfaceIndex,
										  message->data,
										  uint16(message->length & 0xFFFF),
										  0);

		return ret == message->length;
	}

	std::unique_ptr<DeviceLibusb::HandleLock> DeviceLibusb::AquireHandleLock()
	{
		return std::make_unique<HandleLock>(&m_libusbHandle,
											m_handleMutex,
											m_handleInUseCounter,
											m_handleInUseCounterDecremented,
											*this);
	}

	DeviceLibusb::HandleLock::HandleLock(libusb_device_handle** handle,
										 std::mutex& handleMutex,
										 std::atomic<sint32>& handleInUseCounter,
										 std::condition_variable& handleInUseCounterDecremented,
										 DeviceLibusb& device)
		: m_handle(nullptr),
		  m_handleMutex(handleMutex),
		  m_handleInUseCounter(handleInUseCounter),
		  m_handleInUseCounterDecremented(handleInUseCounterDecremented)
	{
		std::lock_guard<std::mutex> lock(handleMutex);
		if (device.IsOpened() && handle != nullptr && handleInUseCounter >= 0)
		{
			this->m_handle = *handle;
			this->m_handleInUseCounter++;
		}
	}

	DeviceLibusb::HandleLock::~HandleLock()
	{
		if (IsValid())
		{
			std::lock_guard<std::mutex> lock(m_handleMutex);
			m_handleInUseCounter--;
			m_handleInUseCounterDecremented.notify_all();
		}
	}

	bool DeviceLibusb::HandleLock::IsValid()
	{
		return m_handle != nullptr;
	}

	libusb_device_handle* DeviceLibusb::HandleLock::GetHandle()
	{
		return m_handle;
	}
} // namespace nsyshid::backend::libusb
