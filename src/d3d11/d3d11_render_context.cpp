/* render_context.cpp
*
* Copyright (C) 2017 Edoardo Dominici
*
* This software may be modified and distributed under the terms
* of the MIT license.  See the LICENSE file for details.
*/
// Header
#include <camy/render_context.hpp>

#if defined(CAMY_OS_WINDOWS) && defined(CAMY_BACKEND_D3D11)

// camy
#include <camy/command_list.hpp>
#include <camy/containers/linear_vector.hpp>

// d3d11
#include <d3d11_1.h>
#include <d3dcommon.h>

template <typename ComType>
void safe_release_com(ComType*& ptr)
{
	if (ptr != nullptr)
		ptr->Release();
	ptr = nullptr;
}

void set_debug_name(ID3D11DeviceChild* child, const camy::char8* type, const camy::char8* name, const camy::char8* gen = nullptr, int idx = 0)
{
	if (child == nullptr)
	{
		CL_ERR("Invalid argument: child is null");
		return;
	}

	camy::StaticString<255> debug_name = type;
	debug_name.append("::");
	debug_name.append(name);
	if (gen != nullptr)
	{
		char idx_buf[3];
		itoa(idx, idx_buf, 10);

		debug_name.append(" __gen(");
		debug_name.append(gen);
		debug_name.append(idx_buf);
		debug_name.append(")");
	}
	child->SetPrivateData(WKPDID_D3DDebugObjectName, debug_name.len(), debug_name.str());
}

namespace camy
{
	RenderContext::RenderContext() { }

	RenderContext::~RenderContext() { destroy(); }

	bool RenderContext::init(const StartupInfo& info_in, RuntimeInfo& info_out)
	{
		destroy();

		CL_INFO("Creating RenderContext..");
		CL_INFO("Num concurrent contexts: ", API::MAX_CONTEXTS);

		HRESULT result = S_OK;
		result = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&m_data.factory);
		if (FAILED(result))
		{
			CL_ERR("DXGICreateFactory failed with error: ", result);
			return false;
		}

		// Trying to create preferred device
		IDXGIAdapter* current_adapter = nullptr;
		if (m_data.factory->EnumAdapters(info_in.preferred_device, &current_adapter) != DXGI_ERROR_NOT_FOUND)
		{
			D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_1;

			UINT flags = 0x0;
#if defined(camy_enable_layers_validation)
			flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

			D3D_FEATURE_LEVEL fl;
			result = D3D11CreateDevice(current_adapter, D3D_DRIVER_TYPE_UNKNOWN,
				nullptr, flags, &feature_level, 1, D3D11_SDK_VERSION,
				&m_data.device_old, &fl, &m_data.immediate_context_old);

			if (SUCCEEDED(result))
			{
				result = m_data.device_old->QueryInterface(__uuidof(ID3D11Device1),
					(void**)&m_data.device);

				if (SUCCEEDED(result))
				{
					m_data.immediate_context_old->QueryInterface(
						__uuidof(ID3D11DeviceContext1),
						(void**)&m_data.immediate_context);

					m_data.adapter = current_adapter;

					goto success;
				}

				// Device doesn't have 11.1 interface
				m_data.device->Release();
			}

			// Invalid adapter
			current_adapter->Release();
		}
		else
			CL_WARN("Failed to create adapter on device: ", info_in.preferred_device);

		uint32 adapter_idx = 0;
		while (m_data.factory->EnumAdapters(adapter_idx, &current_adapter) != DXGI_ERROR_NOT_FOUND)
		{
			D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_1;
	
			UINT flags = 0x0;
#if defined(camy_enable_layers_validation)
			flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
			
			D3D_FEATURE_LEVEL fl;
			result = D3D11CreateDevice(current_adapter, D3D_DRIVER_TYPE_UNKNOWN,
				nullptr, flags, &feature_level, 1, D3D11_SDK_VERSION,
				&m_data.device_old, &fl, &m_data.immediate_context_old);

			if (SUCCEEDED(result))
			{
				result = m_data.device_old->QueryInterface(__uuidof(ID3D11Device1),
					(void**)&m_data.device);
				
				if (SUCCEEDED(result))
				{
					m_data.immediate_context_old->QueryInterface(
						__uuidof(ID3D11DeviceContext1),
						(void**)&m_data.immediate_context);

					m_data.adapter = current_adapter;

					goto success;
				}

				// Device doesn't have 11.1 interface
				m_data.device->Release();
			}

			// Invalid adapter
			current_adapter->Release();
			++adapter_idx;
		}

		CL_ERR("Failed to find valid D3D11.1 Capable context");
		return false;

	success:
		RECT window_rect;
		GetClientRect((HWND)info_out.whandle, &window_rect); // GetWindowRect is not really correct and causes issued w/ alignment

		DXGI_SWAP_CHAIN_DESC sc_desc;
		sc_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sc_desc.BufferDesc.Width = window_rect.right - window_rect.left;
		sc_desc.BufferDesc.Height = window_rect.bottom - window_rect.top;
		sc_desc.BufferDesc.RefreshRate.Numerator = 1;
		sc_desc.BufferDesc.RefreshRate.Denominator = 60;
		sc_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
		sc_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;

		UINT msaa = 1;
		UINT quality = 0;
		if (info_in.msaa > 1)
		{
			UINT rquality;
			m_data.device->CheckMultisampleQualityLevels(sc_desc.BufferDesc.Format, info_in.msaa, &rquality);
			if (rquality == 0)
			{
				CL_WARN("MSAA Level: ", info_in.msaa, " not supported, falling back to 1x");
			}
			else
			{
				msaa = info_in.msaa;
				quality = rquality - 1;
			}
		}

		sc_desc.SampleDesc.Count = msaa;
		sc_desc.SampleDesc.Quality = quality;

		sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
		sc_desc.BufferCount = 1;
		sc_desc.OutputWindow = (HWND)info_out.whandle;
		sc_desc.Windowed = true;
		sc_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		sc_desc.Flags = 0;

		result = m_data.factory->CreateSwapChain(m_data.device, &sc_desc, &m_data.swap_chain);
		if (FAILED(result))
		{
			destroy();
			CL_ERR("IDXGIFactory::CreateSwapChain failed with error: ", result);
			return false;
		}

		Surface backbuffer;
		result = m_data.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer.native.texture2d);
		if (FAILED(result))
		{
			destroy();
			CL_ERR("IDXGIFactory::GetBuffer failed with error: ", result);
			return false;
		}

		backbuffer.native.rtvs = API::tallocate_array<ID3D11RenderTargetView*>(CAMY_UALLOC(1), nullptr);

		result = m_data.device->CreateRenderTargetView(backbuffer.native.texture2d, nullptr, &backbuffer.native.rtvs[0]);
		if (FAILED(result))
		{
			destroy();
			CL_ERR("ID3D11Device::CreateRenderTargetView with error: ", result);
			return false;
		}

		backbuffer.desc.width = sc_desc.BufferDesc.Width;
		backbuffer.desc.height = sc_desc.BufferDesc.Height;
		backbuffer.desc.msaa_levels = sc_desc.SampleDesc.Count;

		// Finally registering backbuffer
		m_backbuffer_handle = m_resource_manager.allocate<Surface>();
		m_resource_manager.get<Surface>(m_backbuffer_handle) = backbuffer;

		for (int i = 0; i < API::MAX_CONTEXTS; ++i)
		{
			m_data.device->CreateDeferredContext1(0, &m_data.contexts[i].deferred_ctx);
			m_data.contexts[i].locked = false;
		}
		m_data.avail_contexts = API::MAX_CONTEXTS;

		// Filling out information
		info_out.backend = "D3D11.1";

		DXGI_ADAPTER_DESC adapter_desc;
		m_data.adapter->GetDesc(&adapter_desc);
		
		// It sucks I know 
		uint32 i = 0;
		while (i < info_out.gpu_name.max_len() && adapter_desc.Description[i] != (WCHAR)'\0')
		{
			info_out.gpu_name.append((char)adapter_desc.Description[i]);
			++i;
		}

		info_out.dedicated_memory = (uint32)adapter_desc.DedicatedVideoMemory / (1024*1024);

		CL_INFO("Created RenderContext: ");
		CL_INFO("Backend: ", info_out.backend);
		CL_INFO("Adapter: ", info_out.gpu_name);
		CL_INFO("VRAM: ", info_out.dedicated_memory, "MB");

		return true;
	}

	void RenderContext::destroy()
	{

	}

	bool RenderContext::acquire(ContextID ctx_id)
	{
		CAMY_ASSERT(m_data.is_valid());

		if (ctx_id >= API::MAX_CONTEXTS)
		{
			CL_ERR("Invalid argument: ctx_id allowed range is [0, ", API::MAX_CONTEXTS, "(API::MAX_CONTEXTS)]");
			return false;
		}

		uint32 des = 1;
		if (API::atomic_cas(m_data.contexts[ctx_id].locked, 0, des) == 0)
		{
			m_data.contexts[ctx_id].owner = API::thread_current();
			CL_INFO("Acquired context: ", ctx_id, " on thread: ", API::thread_current());
			return true;
		}

		CL_ERR("Failed to acquire context: ", ctx_id, " on thread: ", API::thread_current());
		return false;
	}

	void RenderContext::release(ContextID ctx_id)
	{
		CAMY_ASSERT(m_data.is_valid());

		if (ctx_id >= API::MAX_CONTEXTS)
		{
			CL_ERR("Invalid argument: ctx_id allowed range is [0, ", API::MAX_CONTEXTS, "(API::MAX_CONTEXTS)]");
			return;
		}

		uint32 des = 0;
		if (API::atomic_cas(m_data.contexts[ctx_id].locked, 1, des)  == 1)
			return;

		CL_ERR("Failed to acquire context: ", ctx_id, " on thread: ", API::thread_current());
	}

	ContextID RenderContext::id_for_current()
	{
		ThreadID cur_id = API::thread_current();

		for (uint32 i = 0; i < API::MAX_CONTEXTS; ++i)
		{
			if (m_data.contexts[i].locked && m_data.contexts[i].owner == cur_id)
				return i;
		}

		return API::INVALID_CONTEXT_ID;
	}

	RenderContextData& RenderContext::get_platform_data()
	{
		return m_data;
	}

	void RenderContext::flush(CommandList& command_list)
	{
		CAMY_ASSERT(m_data.is_valid());

		if (command_list.m_data.command_list == nullptr)
			return;

		// Uploading cbuffer data
		for (rsize i = 0; i < command_list.m_updates.count(); ++i)
		{
			D3D11_MAPPED_SUBRESOURCE mapped_cbuffer;

			CommandList::ConstantBufferUpdate& update = command_list.m_updates[i];
			ID3D11Buffer* cbuffer = get_constant_buffer(update.handle).native.buffer;

			m_data.immediate_context->Map(cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_cbuffer);

			// TODO: Is it worth for the last one to be partial ?
			std::memcpy(mapped_cbuffer.pData, update.data, update.bytes);

			m_data.immediate_context->Unmap(cbuffer, 0);
		}

		// Actually executing the command list
		m_data.immediate_context->ExecuteCommandList(command_list.m_data.command_list, false);   
		safe_release_com(command_list.m_data.command_list);
	}

	HResource RenderContext::get_backbuffer_handle() const
	{
		return m_backbuffer_handle;
	}

	Surface& RenderContext::get_backbuffer()
	{
		return m_resource_manager.get<Surface>(m_backbuffer_handle);
	}

	void  RenderContext::swap_buffers()
	{
		CAMY_ASSERT(m_data.is_valid());
		m_data.swap_chain->Present(1, 0);
	}

	DXGI_FORMAT camy_to_dxgi(PixelFormat format)
	{
		switch (format)
		{
		case PixelFormat::Unknown:
			return DXGI_FORMAT_UNKNOWN;

			// Compressed formats
		case PixelFormat::BC1Unorm:
			return DXGI_FORMAT_BC1_UNORM;
		case PixelFormat::BC3Unorm:
			return DXGI_FORMAT_BC3_UNORM;
		case PixelFormat::BC5Unorm:
			return DXGI_FORMAT_BC5_UNORM;

			// Typeless formats
		case PixelFormat::R8Typeless:
			return DXGI_FORMAT_R8_TYPELESS;
		case PixelFormat::R16Typeless:
			return DXGI_FORMAT_R16_TYPELESS;
		case PixelFormat::R32Typeless:
			return DXGI_FORMAT_R32_TYPELESS;
		case PixelFormat::R24G8Typeless:
			return DXGI_FORMAT_R24G8_TYPELESS;
		case PixelFormat::R24UnormX8Typeless:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

			// -> Single channel uncompressed
		case PixelFormat::R8Unorm:
			return DXGI_FORMAT_R8_UNORM;
		case PixelFormat::R16Unorm:
			return DXGI_FORMAT_R16_UNORM;
		case PixelFormat::R16Float:
			return DXGI_FORMAT_R16_FLOAT;
		case PixelFormat::R32Float:
			return DXGI_FORMAT_R32_FLOAT;

			// -> Two channels uncompressed
		case PixelFormat::RG8Unorm:
			return DXGI_FORMAT_R8G8_UNORM;
		case PixelFormat::RG16Unorm:
			return DXGI_FORMAT_R16G16_UNORM;
		case PixelFormat::RG16Float:
			return DXGI_FORMAT_R16G16_FLOAT;
		case PixelFormat::RG32Float:
			return DXGI_FORMAT_R32G32_FLOAT;

			// -> Four channels uncompressed
		case PixelFormat::RGBA8Unorm:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case PixelFormat::RGBA16Float:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case PixelFormat::RGBA32Float:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;

			// Depth formats
		case PixelFormat::D16Unorm:
			return DXGI_FORMAT_D16_UNORM;
		case PixelFormat::D32Float:
			return DXGI_FORMAT_D32_FLOAT;
		case PixelFormat::D24UNorm_S8Uint:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;

		default:
			CL_WARN("Failed to translate format to D3D11.1, not supported: ", (uint32)format);
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	HResource RenderContext::create_surface(const SurfaceDesc& desc, const SubSurface* subsurfaces, rsize num_subsurfaces, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		HResource ret;
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;

		D3D11_TEXTURE2D_DESC ndesc{ 0 };
		ndesc.Width = desc.width;
		ndesc.Height = desc.height;

		if (desc.type == SurfaceDesc::Type::Surface2D)
		{
			if (desc.array_count > 1)
				CL_WARN("Creating Surface2D with greater than 1 surface count, use Surface2DArray as type: ", name);
			ndesc.ArraySize = 1;
		}
		else if (desc.type == SurfaceDesc::Type::Surface2DArray)
		{
			if (desc.array_count == 1)
				CL_WARN("Creating Surface2DArray with 1 surface count, you might want to use Surface2D: ", name);
			ndesc.ArraySize = desc.array_count;
		}
		else if (desc.type == SurfaceDesc::Type::SurfaceCube)
		{
			if (desc.array_count > 1)
				CL_WARN("Creating SurfaceCube with greater than 1 surface count, use SurfaceCubeArray as type: ", name);
			ndesc.ArraySize =       6;
		}
		else if (desc.type == SurfaceDesc::Type::SurfaceCubeArray)
		{
			if (desc.array_count == 1)
				CL_WARN("Creating SurfaceCubeArray with 1 surface count, you might want to use SurfaceCube: ", name);
			ndesc.ArraySize = 6 * desc.array_count;
		}

		ndesc.Format = camy_to_dxgi(desc.pixel_format);
		UINT msaa = 1;
		UINT quality = 0;
		if (desc.msaa_levels > 1)
		{
			UINT rquality;
			m_data.device->CheckMultisampleQualityLevels(ndesc.Format, desc.msaa_levels, &rquality);
			if (rquality == 0)
			{
				CL_WARN("MSAA Level: ", desc.msaa_levels, " not supported, falling back to 1x");
			}
			else
			{
				msaa = desc.msaa_levels;
				quality = rquality - 1;
			}
		}

		ndesc.SampleDesc.Count = msaa;
		ndesc.SampleDesc.Quality = quality;

		ndesc.MipLevels = desc.mip_levels;
		ndesc.Usage = desc.usage == Usage::Dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		ndesc.CPUAccessFlags = desc.usage == Usage::Dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
		ndesc.BindFlags = 0;
		if (desc.gpu_views & GPUView::ShaderResource)
			ndesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
		if (desc.gpu_views & GPUView::RenderTarget)
			ndesc.BindFlags |= D3D11_BIND_RENDER_TARGET;
		if (desc.gpu_views & GPUView::DepthStencil)
			ndesc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
		if (desc.gpu_views & GPUView::UnorderedAccess)
			ndesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		ndesc.MiscFlags = 0;
		if (desc.type == SurfaceDesc::Type::SurfaceCube ||
			desc.type == SurfaceDesc::Type::SurfaceCubeArray)
			ndesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		uint32 num_subresources = ndesc.MipLevels;
		if (desc.type == SurfaceDesc::Type::SurfaceCube)
			num_subresources *= 6;
		if (desc.type == SurfaceDesc::Type::SurfaceCubeArray ||
			desc.type == SurfaceDesc::Type::Surface2DArray)
			num_subresources *= desc.array_count;

		bool upload_data = false;
		LinearVector<D3D11_SUBRESOURCE_DATA> sub_resources(num_subresources);
		if (subsurfaces != nullptr)
		{
			if (num_subsurfaces != num_subresources)
			{
				CL_ERR("Invalid argument: num_subsurfaces: ", num_subsurfaces, " expected: ", num_subresources);
			}
			else
			{
				for (uint32 i = 0; i < num_subresources; ++i)
				{
					D3D11_SUBRESOURCE_DATA& next = sub_resources.next();
					next.pSysMem = subsurfaces[i].data;
					next.SysMemPitch = subsurfaces[i].pitch;
					next.SysMemSlicePitch = 0;
				}
				upload_data = true;
			}
		}

		// Creating views
		uint32 num_views = 1; // 1 for Type::Surface2D
		if (desc.type == SurfaceDesc::Type::Surface2DArray)
			num_views = 1 + desc.array_count; // 1 for the whole resource + 1 for each element
		else if (desc.type == SurfaceDesc::Type::SurfaceCube)
			num_views = 1 + 6; // 1 for the whole resource + 1 for each face
		else if (desc.type == SurfaceDesc::Type::SurfaceCubeArray)
			num_views = 1 + 6 * desc.array_count; // TODO: check this

		ID3D11ShaderResourceView** srvs = nullptr;
		ID3D11RenderTargetView** rtvs = nullptr;
		ID3D11UnorderedAccessView** uavs = nullptr;
		ID3D11DepthStencilView** dsvs = nullptr;

		HRESULT result = m_data.device->CreateTexture2D(&ndesc, upload_data ? sub_resources.data() : nullptr, &texture);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateTexture2D failed with error: ", result);
			goto error;
		}
		set_debug_name(texture, "Surface", name);

		if (desc.gpu_views & GPUView::ShaderResource)
		{
			srvs = API::tallocate_array<ID3D11ShaderResourceView*>(CAMY_UALLOC(num_views), nullptr);

			// Whole resource
			result = m_data.device->CreateShaderResourceView(texture, nullptr, &srvs[0]);
			if (FAILED(result))
			{
				CL_ERR("ID3D11Device::CreateShaderResourceView failed with error: ", result);
				goto error;
			}
			set_debug_name(srvs[0],"Surface", name, "SRV");

			// Per element
			D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
			ZeroMemory(&srv_desc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
			srv_desc.Format = camy_to_dxgi(desc.pixel_format_srv);

			for (uint32 i = 0; i < num_views - 1; ++i)
			{
				switch (desc.type)
				{
				case SurfaceDesc::Type::Surface2D:
					CAMY_ASSERT("Check num_views generation");
					break;
				case SurfaceDesc::Type::SurfaceCube:
				case SurfaceDesc::Type::Surface2DArray:
					srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
					srv_desc.Texture2DArray.MostDetailedMip = 0;
					srv_desc.Texture2DArray.MipLevels = desc.mip_levels;
					srv_desc.Texture2DArray.FirstArraySlice = i;
					srv_desc.Texture2DArray.ArraySize = 1;

					break;
				case SurfaceDesc::Type::SurfaceCubeArray:
					srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
					srv_desc.TextureCubeArray.MostDetailedMip = 0;
					srv_desc.TextureCubeArray.MipLevels = desc.mip_levels;
					srv_desc.TextureCubeArray.First2DArrayFace = i;
					srv_desc.TextureCubeArray.NumCubes = 1;
					break;
				}
				result = m_data.device->CreateShaderResourceView(texture, &srv_desc, &srvs[1 + i]);
				if (FAILED(result))
				{
					CL_ERR("ID3D11Device::CreateShaderResourceView failed with error: ", result);
					goto error;
				}
				set_debug_name(srvs[1 + i], "Surface", name, "SRV", 1 + i);
			}
		}

		if (desc.gpu_views & GPUView::RenderTarget)
		{
			rtvs = API::tallocate_array<ID3D11RenderTargetView*>(CAMY_UALLOC(num_views), nullptr);

			// Whole resource
			result = m_data.device->CreateRenderTargetView(texture, nullptr, &rtvs[0]);
			if (FAILED(result))
			{
				CL_ERR("ID3D11Device::CreateRenderTargetView failed with error: ", result);
				goto error;
			}
			set_debug_name(rtvs[0], "Surface", name, "RTV");

			// Per element
			D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
			ZeroMemory(&rtv_desc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
			rtv_desc.Format = camy_to_dxgi(desc.pixel_format_rtv);

			for (uint32 i = 0; i < num_views - 1; ++i)
			{
				switch (desc.type)
				{
				case SurfaceDesc::Type::Surface2D:
					rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
					rtv_desc.Texture2D.MipSlice = 0;
					break;
				case SurfaceDesc::Type::SurfaceCubeArray:
				case SurfaceDesc::Type::Surface2DArray:
				case SurfaceDesc::Type::SurfaceCube:
					// TODO: have yet to test surfacecubearray
					rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					rtv_desc.Texture2DArray.MipSlice = 0;
					rtv_desc.Texture2DArray.FirstArraySlice = i;
					rtv_desc.Texture2DArray.ArraySize = 1;
					break;
				default:
					CAMY_ASSERT(false);
				}

				result = m_data.device->CreateRenderTargetView(texture, &rtv_desc, &rtvs[1 + i]);
				if (FAILED(result))
				{
					CL_ERR("ID3D11Device::CreateRenderTargetView failed with error: ", result);
					goto error;
				}
				set_debug_name(rtvs[1 + i], "Surface", name, "RTV", 1 + i);
			}
		}

		if (desc.gpu_views & GPUView::UnorderedAccess)
		{
			uavs = API::tallocate_array<ID3D11UnorderedAccessView*>(CAMY_UALLOC(num_views), nullptr);

			// Whole resource
			result = m_data.device->CreateUnorderedAccessView(texture, nullptr, &uavs[0]);
			if (FAILED(result))
			{
				CL_ERR("ID3D11Device::CreateUnorderedAccessView with error: ", result);
				goto error;
			}
			set_debug_name(uavs[0], "Surface", name, "UAV");

			// Per element
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;
			ZeroMemory(&uav_desc, sizeof(D3D11_UNORDERED_ACCESS_VIEW_DESC));
			uav_desc.Format = camy_to_dxgi(desc.pixel_format_uav);
			for (uint32 i = 0; i < num_views - 1; ++i)
			{
				switch (desc.type)
				{
				case SurfaceDesc::Type::Surface2D:
					uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					uav_desc.Texture2D.MipSlice = 0;
					break;
				case SurfaceDesc::Type::Surface2DArray:
				case SurfaceDesc::Type::SurfaceCube:
				case SurfaceDesc::Type::SurfaceCubeArray: // TODO: have yet to test surfacecubearray
					uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
					uav_desc.Texture2DArray.MipSlice = 0;
					uav_desc.Texture2DArray.FirstArraySlice = i;
					uav_desc.Texture2DArray.ArraySize = 1;
				}

				result = m_data.device->CreateUnorderedAccessView(texture, &uav_desc, &uavs[1 + i]);
				if (FAILED(result))
				{
					CL_ERR("ID3D11Device::CreateUnorderedAccessView with failed: ", result);
					goto error;
				}
				set_debug_name(uavs[1 + i], "Surface", name, "UAV", 1 + i);
			}
		}

		if (desc.gpu_views & GPUView::DepthStencil)
		{
			dsvs = API::tallocate_array<ID3D11DepthStencilView*>(CAMY_UALLOC(num_views), nullptr);

			// Whole resource
			result = m_data.device->CreateDepthStencilView(texture, nullptr, &dsvs[0]);
			if (FAILED(result))
			{
				CL_ERR("ID3D11Device::CreateDepthStencilView failed with error: ", result);
				goto error;
			}
			set_debug_name(dsvs[0], "Surface", name, "DSV");

			// Per element
			D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc;
			ZeroMemory(&dsv_desc, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));
			dsv_desc.Format = camy_to_dxgi(desc.pixel_format_uav);
			for (uint32 i = 0; i < num_views - 1; ++i)
			{
				switch (desc.type)
				{
				case SurfaceDesc::Type::Surface2D:
					dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
					dsv_desc.Texture2D.MipSlice = 0;
					break;
				case SurfaceDesc::Type::Surface2DArray:
				case SurfaceDesc::Type::SurfaceCube:
				case SurfaceDesc::Type::SurfaceCubeArray: // TODO: have yet to test surfacecubearray
					dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
					dsv_desc.Texture2DArray.MipSlice = 0;
					dsv_desc.Texture2DArray.FirstArraySlice = i;
					dsv_desc.Texture2DArray.ArraySize = 1;
				}

				result = m_data.device->CreateDepthStencilView(texture, &dsv_desc, &dsvs[1 + i]);
				if (FAILED(result))
				{
					CL_ERR("ID3D11Device::CreateDepthStencilView failed with error: ", result);
					goto error;
				}
				set_debug_name(dsvs[1 + i], "Surface", name, "DSV", 1 + i);
			}
		}

		ret = m_resource_manager.allocate<Surface>();
		Surface& res = m_resource_manager.get<Surface>(ret);

		res.desc = desc;
		res.desc.msaa_levels = ndesc.SampleDesc.Count;
		res.native.texture2d = texture;
		res.native.srvs = srvs;
		res.native.rtvs = rtvs;
		res.native.uavs = uavs;
		res.native.dsvs = dsvs;

		CL_INFO("Created Surface: ", name, "[", desc.width, "x", desc.height, "]");
		return ret;

	error:
		API::tdeallocate(dsvs);
		API::tdeallocate(uavs);
		API::tdeallocate(rtvs);
		API::tdeallocate(srvs);
		safe_release_com(texture);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_buffer(const BufferDesc& desc, const void* data, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		if (desc.is_uav == true && desc.usage == Usage::Dynamic)
		{
			CL_ERR("Can't create a dynamic buffer with GPUView_UnorderedAccess");
			return HResource::make_invalid();
		}

		HResource ret;
		ID3D11Buffer* buffer = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;

		D3D11_BUFFER_DESC ndesc;
		ndesc.ByteWidth = desc.num_elements * desc.element_size;
		ndesc.Usage = D3D11_USAGE_DYNAMIC;
		ndesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (desc.is_uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
		ndesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ndesc.StructureByteStride = desc.element_size;

		UINT misc_flags = 0;
		if (desc.type == BufferDesc::Type::Structured)
			misc_flags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		ndesc.MiscFlags = misc_flags;

		D3D11_SUBRESOURCE_DATA initial_data;
		initial_data.pSysMem = data;
		initial_data.SysMemPitch = 0;

		HRESULT result = m_data.device->CreateBuffer(&ndesc, data == nullptr ? nullptr : &initial_data, &buffer);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateBuffer failed with error: ", result);
			goto error;
		}
		set_debug_name(buffer, "Buffer", name);

		// Shader Resource View
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
		srv_desc.Format = DXGI_FORMAT_UNKNOWN;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srv_desc.Buffer.ElementOffset = 0;
		srv_desc.Buffer.NumElements = desc.num_elements;

		result = m_data.device->CreateShaderResourceView(buffer, &srv_desc, &srv);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateShaderResourceView failed with error: ", result);
			goto error;
		}
		set_debug_name(srv, "Buffer", name, "SRV");

		// Unordered access view
		if (desc.is_uav)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;
			uav_desc.Format = DXGI_FORMAT_UNKNOWN;
			uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav_desc.Buffer.FirstElement = 0;
			uav_desc.Buffer.NumElements = desc.num_elements;
			uav_desc.Buffer.Flags = 0;

			result = m_data.device->CreateUnorderedAccessView(buffer, &uav_desc, &uav);
			if (FAILED(result))
			{
				CL_ERR("ID3D11Device::CreateUnorderedAccessView failed with error: ", result);
				goto error;
			}
			set_debug_name(srv, "Buffer", name, "UAV");
		}

		ret = m_resource_manager.allocate<Buffer>();
		Buffer& res = m_resource_manager.get<Buffer>(ret);

		res.desc = desc;
		res.native.buffer = buffer;
		res.native.srv = srv;
		res.native.uav = uav;

		CL_INFO("Created Buffer: ", name, "[", desc.element_size, "x", desc.num_elements, "]");
		return ret;

	error:
		safe_release_com(uav);
		safe_release_com(srv);
		safe_release_com(buffer);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_vertex_buffer(const VertexBufferDesc& desc, const void* data, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		HResource ret;
		ID3D11Buffer* buffer = nullptr;

		D3D11_BUFFER_DESC ndesc;
		ndesc.ByteWidth = desc.num_elements * desc.element_size;
		ndesc.Usage = desc.usage == Usage::Dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		ndesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		ndesc.CPUAccessFlags = desc.usage == Usage::Dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
		ndesc.MiscFlags = 0;
		ndesc.StructureByteStride = 0;

		D3D11_SUBRESOURCE_DATA vb_data;
		vb_data.pSysMem = data;
		vb_data.SysMemPitch = vb_data.SysMemSlicePitch = 0;

		HRESULT result = m_data.device->CreateBuffer(&ndesc, (data != nullptr) ? &vb_data : nullptr, &buffer);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateBuffer failed with error: ", result);
			goto error;
		}
		set_debug_name(buffer, "VertexBuffer", name);

		ret = m_resource_manager.allocate<VertexBuffer>();
		VertexBuffer& res = m_resource_manager.get<VertexBuffer>(ret);

		res.desc = desc;
		res.native.buffer = buffer;
		res.native.stride = desc.element_size;

		CL_INFO("Created VertexBuffer: ", name);
		return ret;

	error:
		safe_release_com(buffer);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_index_buffer(const IndexBufferDesc& desc, const void* data, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		HResource ret;
		ID3D11Buffer* buffer = nullptr;
		rsize element_size = 2;
		if (desc.extended32)
			element_size = 4;

		D3D11_BUFFER_DESC ndesc;
		ndesc.ByteWidth = desc.num_elements * element_size;
		ndesc.Usage = desc.usage == Usage::Dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		ndesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		ndesc.CPUAccessFlags = desc.usage == Usage::Dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
		ndesc.MiscFlags = 0;
		ndesc.StructureByteStride = 0;

		D3D11_SUBRESOURCE_DATA vb_data;
		vb_data.pSysMem = data;
		vb_data.SysMemPitch = vb_data.SysMemSlicePitch = 0;

		HRESULT result = m_data.device->CreateBuffer(&ndesc, (data != nullptr) ? &vb_data : nullptr, &buffer);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateBuffer failed with error: ", result);
			goto error;
		}
		set_debug_name(buffer, "IndexBuffer", name);

		ret = m_resource_manager.allocate<IndexBuffer>();
		IndexBuffer& res = m_resource_manager.get<IndexBuffer>(ret);

		res.desc = desc;
		res.native.buffer = buffer;
		if (!desc.extended32)
			res.native.dxgi_format = DXGI_FORMAT_R16_UINT;
		else
			res.native.dxgi_format = DXGI_FORMAT_R32_UINT;

		CL_INFO("Created IndexBuffer: ", name, "[", element_size, "x", desc.num_elements, "]");
		return ret;

	error:
		safe_release_com(buffer);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_constant_buffer(const ConstantBufferDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		HResource ret;
		ID3D11Buffer* buffer = nullptr;

		D3D11_BUFFER_DESC ndesc;
		ndesc.ByteWidth = desc.size;
		ndesc.Usage = D3D11_USAGE_DYNAMIC;
		ndesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ndesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ndesc.MiscFlags = 0;
		ndesc.StructureByteStride = 0;

		HRESULT result = m_data.device->CreateBuffer(&ndesc, nullptr, &buffer);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateBuffer failed with error: ", result);
			goto error;
		}
		set_debug_name(buffer, "ConstantBuffer", name);

		ret = m_resource_manager.allocate<ConstantBuffer>();
		ConstantBuffer& res = m_resource_manager.get<ConstantBuffer>(ret);
		
		res.desc = desc;
		res.native.buffer = buffer;

		CL_INFO("Created ConstantBuffer: ", name, "[", desc.size, "]");
		return ret;
	error:
		safe_release_com(buffer);
		return HResource::make_invalid();
	}
	
	void compile_from_camy(BlendStateDesc::Type blend_mode, D3D11_BLEND_DESC& bs_desc)
	{
		bs_desc.AlphaToCoverageEnable = false; // NO MSAA support yet
		bs_desc.IndependentBlendEnable = false; // Not supported yet
		bs_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		switch (blend_mode)
		{
		case BlendStateDesc::Type::Opaque:
			bs_desc.RenderTarget[0].BlendEnable = true;
			bs_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bs_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
			bs_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bs_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			break;

		case BlendStateDesc::Type::Transparent:
			bs_desc.RenderTarget[0].BlendEnable = true;
			bs_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bs_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bs_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bs_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			break;

		case BlendStateDesc::Type::Additive:
			bs_desc.RenderTarget[0].BlendEnable = true;
			bs_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bs_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bs_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
			bs_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bs_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			break;
		default:
			CAMY_ASSERT(false);
		}
	}

	D3D11_FILL_MODE camy_to_d3d11(RasterizerStateDesc::Fill fill_mode)
	{
		switch (fill_mode)
		{
		case RasterizerStateDesc::Fill::Solid:
			return D3D11_FILL_SOLID;
		case RasterizerStateDesc::Fill::Wireframe:
			return D3D11_FILL_WIREFRAME;
		default:
			return D3D11_FILL_SOLID;
		}
	}

	D3D11_CULL_MODE camy_to_d3d11(RasterizerStateDesc::Cull cull_mode)
	{
		switch (cull_mode)
		{
		case RasterizerStateDesc::Cull::Back:
			return D3D11_CULL_BACK;
		case RasterizerStateDesc::Cull::Front:
			return D3D11_CULL_FRONT;
		case RasterizerStateDesc::Cull::None:
			return D3D11_CULL_NONE;
		default:
			return D3D11_CULL_NONE; // Default d3d11 is back but None IMHO Makes the user notice that there is something wrong :)
		}
	}

	HResource RenderContext::create_blend_state(const BlendStateDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.is_valid());

		D3D11_BLEND_DESC bs_desc;
		compile_from_camy(desc.type, bs_desc);

		HResource ret;
		ID3D11BlendState* blend_state = nullptr;
		HRESULT result = m_data.device->CreateBlendState(&bs_desc, &blend_state);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateBlendState with error: ", result);
			goto error;
		}
		set_debug_name(blend_state, "BlendState", name);

		ret = m_resource_manager.allocate<BlendState>();
		BlendState& res = m_resource_manager.get<BlendState>(ret);

		res.desc = desc;
		res.native.state = blend_state;

		CL_INFO("Created BlendState");
		return ret;

	error:
		safe_release_com(blend_state);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_rasterizer_state(const RasterizerStateDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.device != nullptr);

		D3D11_RASTERIZER_DESC rs_desc;
		rs_desc.CullMode = camy_to_d3d11(desc.cull);
		rs_desc.FillMode = camy_to_d3d11(desc.fill);
		rs_desc.FrontCounterClockwise = 
		rs_desc.DepthBias = desc.depth_bias;
		rs_desc.DepthBiasClamp = desc.depth_bias_clamp;
		rs_desc.SlopeScaledDepthBias = desc.slope_scaled_depth_bias;
		rs_desc.DepthClipEnable = true;
		rs_desc.ScissorEnable = false;
		rs_desc.MultisampleEnable = true;
		rs_desc.AntialiasedLineEnable = false;

		HResource ret;
		ID3D11RasterizerState* rasterizer_state = nullptr;
		HRESULT result = m_data.device->CreateRasterizerState(&rs_desc, &rasterizer_state);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateRasterizerState with error: ", result);
			goto error;
		}
		set_debug_name(rasterizer_state, "RasterizerState", name);

		ret = m_resource_manager.allocate<RasterizerState>();
		RasterizerState& res = m_resource_manager.get<RasterizerState>(ret);

		res.desc = desc;
		res.native.state = rasterizer_state;

		CL_INFO("Created RasterizerState: ", name);
		return ret;

	error:
		safe_release_com(rasterizer_state);
		return HResource::make_invalid();
	}

	DXGI_FORMAT camy_to_d3d11(InputElement::Type type)
	{
		switch (type)
		{
		case InputElement::Type::UInt:
			return DXGI_FORMAT_R32_UINT;
		case InputElement::Type::Float2:
			return DXGI_FORMAT_R32G32_FLOAT;
		case InputElement::Type::Float3:
			return DXGI_FORMAT_R32G32B32_FLOAT;
		case InputElement::Type::Float4:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	HResource RenderContext::create_input_signature(InputSignatureDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.device != nullptr);

		// Perfectly fine, just hardware generated values
		if (desc.num_elements == 0)
			return HResource::make_invalid();

		LinearVector<D3D11_INPUT_ELEMENT_DESC> input_elements(desc.num_elements);
		for (unsigned int i = 0; i < desc.num_elements; ++i)
		{
			D3D11_INPUT_ELEMENT_DESC& next = input_elements.next();
			next.SemanticName = desc.elements[i].name.str();
			next.SemanticIndex = desc.elements[i].semantic_idx;
			next.InputSlot = desc.elements[i].slot;
			next.Format = camy_to_d3d11(desc.elements[i].type);
			next.AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;

			if (desc.elements[i].is_instanced)
			{
				next.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
				next.InstanceDataStepRate = 1;
			}
			else
			{
				next.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
				next.InstanceDataStepRate = 0;
			}
		}

		HResource ret;
		ID3D11InputLayout* input_layout = nullptr;
		HRESULT result = m_data.device->CreateInputLayout(input_elements.data(), desc.num_elements, desc.bytecode.data, desc.bytecode.size, &input_layout);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateInputLayout failed with error: ", result);
			goto error;
		}
		set_debug_name(input_layout, "InputSignature", name);

		ret = m_resource_manager.allocate<InputSignature>();
		InputSignature& res = m_resource_manager.get<InputSignature>(ret);

		res.desc = desc;
		res.native.input_layout = input_layout;

		CL_INFO("Created InputSignature: ", name);
		return ret;

	error:
		safe_release_com(input_layout);
		return HResource::make_invalid();
	}

	D3D11_TEXTURE_ADDRESS_MODE camy_to_d3d11(SamplerDesc::Address address_mode)
	{
		switch (address_mode)
		{
		case SamplerDesc::Address::Clamp:
			return D3D11_TEXTURE_ADDRESS_CLAMP;
		case SamplerDesc::Address::Wrap:
			return D3D11_TEXTURE_ADDRESS_WRAP;
		case SamplerDesc::Address::Mirror:
			return D3D11_TEXTURE_ADDRESS_MIRROR;
		default:
			return D3D11_TEXTURE_ADDRESS_CLAMP;
		}
	}

	D3D11_FILTER camy_to_d3d11(SamplerDesc::Filter filter, bool comparison)
	{
		switch (filter)
		{
		case SamplerDesc::Filter::Point:
			return comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
		case SamplerDesc::Filter::Linear:
			return comparison ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		case SamplerDesc::Filter::Anisotropic:
			return comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
		default:
			return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		}
	}

	D3D11_COMPARISON_FUNC camy_to_d3d11(SamplerDesc::Comparison comparison)
	{
		switch (comparison)
		{
		case SamplerDesc::Comparison::Less:
			return D3D11_COMPARISON_LESS;
		case SamplerDesc::Comparison::LessEqual:
			return D3D11_COMPARISON_LESS_EQUAL;
		case SamplerDesc::Comparison::Never:
			return D3D11_COMPARISON_NEVER;
		default:
			return D3D11_COMPARISON_NEVER;
		}
	}

	HResource RenderContext::create_sampler(const SamplerDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.device != nullptr);

		D3D11_SAMPLER_DESC s_desc;
		s_desc.Filter = camy_to_d3d11(desc.filter, desc.comparison != SamplerDesc::Comparison::Never);
		s_desc.AddressU =
			s_desc.AddressV =
			s_desc.AddressW = camy_to_d3d11(desc.address);
		s_desc.MinLOD = -FLT_MAX;
		s_desc.MaxLOD = FLT_MAX;
		s_desc.MipLODBias = 0.f;
		s_desc.MaxAnisotropy = desc.filter == SamplerDesc::Filter::Anisotropic ? D3D11_MAX_MAXANISOTROPY : 0;
		s_desc.ComparisonFunc = camy_to_d3d11(desc.comparison);
		s_desc.BorderColor[0] =
			s_desc.BorderColor[1] =
			s_desc.BorderColor[2] =
			s_desc.BorderColor[3] = 1.f;

		HResource ret;
		ID3D11SamplerState* sampler;
		HRESULT result = m_data.device->CreateSamplerState(&s_desc, &sampler);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateSamplerState failed with error: ", result);
			goto error;
		}
		set_debug_name(sampler, "Sampler", name);

		ret = m_resource_manager.allocate<Sampler>();
		Sampler& res = m_resource_manager.get<Sampler>(ret);

		res.desc = desc;
		res.native.sampler = sampler;

		CL_INFO("Created Sampler: ", name);
		return ret;

	error:
		sampler = nullptr;
		return HResource::make_invalid();
	}

	D3D11_COMPARISON_FUNC camy_to_d3d11(DepthStencilStateDesc::DepthFunc func)
	{
		switch (func)
		{
		case DepthStencilStateDesc::DepthFunc::LessEqual:
			return D3D11_COMPARISON_LESS_EQUAL;
		case DepthStencilStateDesc::DepthFunc::Less:
		default:
			return D3D11_COMPARISON_LESS;
		}
	}

	HResource RenderContext::create_depth_stencil_state(const DepthStencilStateDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.device != nullptr);

		D3D11_DEPTH_STENCIL_DESC dss_desc;
		dss_desc.DepthEnable = true;
		dss_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dss_desc.DepthFunc = camy_to_d3d11(desc.depth_func);
		dss_desc.StencilEnable = false;

		HResource ret;
		ID3D11DepthStencilState* depth_stencil_state = nullptr;
		HRESULT result = m_data.device->CreateDepthStencilState(&dss_desc, &depth_stencil_state);
		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateDepthStencilState failed with error: ", result);
			goto error;
		}
		set_debug_name(depth_stencil_state, "DepthStencilState", name);

		ret = m_resource_manager.allocate<DepthStencilState>();
		DepthStencilState& res = m_resource_manager.get<DepthStencilState>(ret);

		res.desc = desc;
		res.native.state = depth_stencil_state;

		CL_INFO("Created DepthStencilState: ", name);
		return ret;

	error:
		safe_release_com(depth_stencil_state);
		return HResource::make_invalid();
	}

	HResource RenderContext::create_shader(const ShaderDesc& desc, const char8* name)
	{
		CAMY_ASSERT(m_data.device != nullptr);

		HResource ret;
		ID3D11DeviceChild* shader = nullptr;
		HRESULT result = S_FALSE;
		switch (desc.type)
		{
		case ShaderDesc::Type::Vertex:
			result = m_data.device->CreateVertexShader(desc.bytecode.data, desc.bytecode.size, nullptr, (ID3D11VertexShader**)&shader);
			break;
		case ShaderDesc::Type::Geometry:
			result = m_data.device->CreateGeometryShader(desc.bytecode.data, desc.bytecode.size, nullptr, (ID3D11GeometryShader**)&shader);
			break;
		case ShaderDesc::Type::Pixel:
			result = m_data.device->CreatePixelShader(desc.bytecode.data, desc.bytecode.size, nullptr, (ID3D11PixelShader**)&shader);
			break;
		case ShaderDesc::Type::Compute:
			result = m_data.device->CreateComputeShader(desc.bytecode.data, desc.bytecode.size, nullptr, (ID3D11ComputeShader**)&shader);
			break;
		default:
			CAMY_ASSERT(false);
		}

		if (FAILED(result))
		{
			CL_ERR("ID3D11Device::CreateShader failed with error: ", result);
			goto error;
		}
		set_debug_name(shader, "Shader", name);

		ret = m_resource_manager.allocate<Shader>();
		Shader& res = m_resource_manager.get<Shader>(ret);

		res.desc = desc;
		res.native.shader = shader;

		CL_INFO("Created Shader: ", name);
		return ret;

	error:
		safe_release_com(shader);
		return HResource::make_invalid();
	}

	void RenderContext::destroy_surface(HResource handle)
	{
		if (handle.is_invalid()) return;
		Surface& surface = m_resource_manager.get<Surface>(handle);

		if (surface.native.srvs != nullptr)
		{
			for (rsize i = 0; i < surface.native.num_views; ++i)
				safe_release_com(surface.native.srvs[i]);
		}

		if (surface.native.rtvs != nullptr)
		{
			for (rsize i = 0; i < surface.native.num_views; ++i)
				safe_release_com(surface.native.rtvs[i]);
		}

		if (surface.native.uavs != nullptr)
		{
			for (rsize i = 0; i < surface.native.num_views; ++i)
				safe_release_com(surface.native.uavs[i]);
		}

		if (surface.native.dsvs != nullptr)
		{
			for (rsize i = 0; i < surface.native.num_views; ++i)
				safe_release_com(surface.native.dsvs[i]);
		}

		m_resource_manager.deallocate<Surface>(handle);
	}

	void RenderContext::destroy_buffer(HResource handle)
	{
		if (handle.is_invalid()) return;
		Buffer& buffer = m_resource_manager.get<Buffer>(handle);

		safe_release_com(buffer.native.uav);
		safe_release_com(buffer.native.srv);
		safe_release_com(buffer.native.buffer);
		
		m_resource_manager.deallocate<Buffer>(handle);
	}

	void RenderContext::destroy_vertex_buffer(HResource handle)
	{
		if (handle.is_invalid()) return;
		VertexBuffer& vertex_buffer = m_resource_manager.get<VertexBuffer>(handle);

		safe_release_com(vertex_buffer.native.buffer);
	
		m_resource_manager.deallocate<VertexBuffer>(handle);
	}

	void RenderContext::destroy_index_buffer(HResource handle)
	{
		if (handle.is_invalid()) return;
		IndexBuffer& index_buffer = m_resource_manager.get<IndexBuffer>(handle);

		safe_release_com(index_buffer.native.buffer);
			
		m_resource_manager.deallocate<IndexBuffer>(handle);
	}

	void RenderContext::destroy_constant_buffer(HResource handle)
	{
		if (handle.is_invalid()) return;
		ConstantBuffer& constant_buffer = m_resource_manager.get<ConstantBuffer>(handle);
		
		safe_release_com(constant_buffer.native.buffer);
	
		m_resource_manager.deallocate<ConstantBuffer>(handle);
	}

	void RenderContext::destroy_blend_state(HResource handle)
	{
		if (handle.is_invalid()) return;
		BlendState& blend_state = m_resource_manager.get<BlendState>(handle);

		safe_release_com(blend_state.native.state);
	
		m_resource_manager.deallocate<BlendState>(handle);
	}

	void RenderContext::destroy_rasterizer_state(HResource handle)
	{
		if (handle.is_invalid()) return;
		RasterizerState& rasterizer_state = m_resource_manager.get<RasterizerState>(handle);

		safe_release_com(rasterizer_state.native.state);
	
		m_resource_manager.deallocate<RasterizerState>(handle);
	}

	void RenderContext::destroy_input_signature(HResource handle)
	{
		if (handle.is_invalid()) return;
		InputSignature& input_signature = m_resource_manager.get<InputSignature>(handle);

		safe_release_com(input_signature.native.input_layout);
		
		m_resource_manager.deallocate<InputSignature>(handle);
	}

	void RenderContext::destroy_sampler(HResource handle)
	{
		if (handle.is_invalid()) return;
		Sampler& sampler = m_resource_manager.get<Sampler>(handle);

		safe_release_com(sampler.native.sampler);
	
		m_resource_manager.deallocate<Sampler>(handle);
	}

	void RenderContext::destroy_depth_stencil_state(HResource handle)
	{
		if (handle.is_invalid()) return;
		DepthStencilState& depth_stencil_state = m_resource_manager.get<DepthStencilState>(handle);

		safe_release_com(depth_stencil_state.native.state);
		
		m_resource_manager.deallocate<DepthStencilState>(handle);
	}

	void RenderContext::destroy_shader(HResource handle)
	{
		if (handle.is_invalid()) return;
		Shader& shader = m_resource_manager.get<Shader>(handle);

		safe_release_com(shader.native.shader);
			
		m_resource_manager.deallocate<Shader>(handle);
	}

	Surface& RenderContext::get_surface(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<Surface>(handle);
	}

	Buffer& RenderContext::get_buffer(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<Buffer>(handle);
	}

	VertexBuffer& RenderContext::get_vertex_buffer(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<VertexBuffer>(handle);
	}

	IndexBuffer& RenderContext::get_index_buffer(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<IndexBuffer>(handle);
	}

	ConstantBuffer& RenderContext::get_constant_buffer(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<ConstantBuffer>(handle);
	}

	BlendState& RenderContext::get_blend_state(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<BlendState>(handle);
	}

	RasterizerState& RenderContext::get_rasterizer_state(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<RasterizerState>(handle);
	}

	InputSignature& RenderContext::get_input_signature(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<InputSignature>(handle);
	}

	Sampler& RenderContext::get_sampler(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<Sampler>(handle);
	}

	DepthStencilState& RenderContext::get_depth_stencil_state(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<DepthStencilState>(handle);
	}

	Shader& RenderContext::get_shader(HResource handle)
	{
		CAMY_ASSERT(handle.is_valid());
		return m_resource_manager.get<Shader>(handle);
	}
}

#endif