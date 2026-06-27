//////////////////////////////////////////////////////////////////////
// The Force Engine — N64 (libdragon) render backend.
//
// Replaces the desktop SDL/OpenGL backend for the software-rendering path.
// The software renderer writes a 320x200 8-bit (CI8) indexed framebuffer plus
// a 256-color palette; this backend blits that to the N64 RGBA16 framebuffer
// each frame using rdpq with a TLUT (palette lookup table).
//
// libdragon's OpenGL is fixed-function GL 1.1 (no shaders, no paletted GL
// textures, 64x64 max GL texture), so the indexed-framebuffer present is done
// with rdpq, which natively supports CI8 + TLUT and auto-tiles large surfaces.
// The GL context (set up in main) is left available for future 3D work.
//////////////////////////////////////////////////////////////////////

#include <libdragon.h>
#include <surface.h>
#include <rdpq.h>
#include <rdpq_tex.h>
#include <rdpq_mode.h>
#include <rdpq_attach.h>
#include <display.h>

#include <TFE_RenderBackend/renderBackend.h>

#include <cstring>

namespace TFE_RenderBackend
{
	// Current CI8 framebuffer (owned by the caller / virtual framebuffer module).
	static const u8* s_ci8Buffer = nullptr;
	static u32 s_vdispWidth  = 320;
	static u32 s_vdispHeight = 200;

	// Display (physical) dimensions.
	static u32 s_displayWidth  = 320;
	static u32 s_displayHeight = 240;

	// Palette converted to RGBA5551 for the RDP TLUT.
	static u16 s_tlut[256] = { 0 };

	bool init(const WindowState& state)
	{
		s_displayWidth  = state.width  ? state.width  : 320;
		s_displayHeight = state.height ? state.height : 240;
		return true;
	}

	void destroy()
	{
		s_ci8Buffer = nullptr;
	}

	bool isWindowMinimized()
	{
		return false;
	}

	void resize(s32 width, s32 height)
	{
		if (width  > 0) { s_displayWidth  = (u32)width;  }
		if (height > 0) { s_displayHeight = (u32)height; }
	}

	void clearWindow()
	{
		surface_t* disp = display_get();
		rdpq_attach_clear(disp, nullptr);
		rdpq_detach_show();
	}

	//////////////////////////////////////////////////
	// Virtual display (software framebuffer)
	//////////////////////////////////////////////////
	bool createVirtualDisplay(const VirtualDisplayInfo& vdispInfo)
	{
		s_vdispWidth  = vdispInfo.width  ? vdispInfo.width  : 320;
		s_vdispHeight = vdispInfo.height ? vdispInfo.height : 200;
		return true;
	}

	void updateVirtualDisplay(const void* buffer, size_t size)
	{
		(void)size;
		s_ci8Buffer = (const u8*)buffer;
	}

	void setPalette(const u32* palette)
	{
		if (!palette) { return; }
		for (s32 i = 0; i < 256; i++)
		{
			// Engine palette is 0xAABBGGRR (R in low byte) -> RGBA5551 for the RDP TLUT.
			const u32 c = palette[i];
			const u32 r = (c      ) & 0xff;
			const u32 g = (c >>  8) & 0xff;
			const u32 b = (c >> 16) & 0xff;
			s_tlut[i] = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1u);
		}
	}

	const u32* getPalette()
	{
		return nullptr;
	}

	//////////////////////////////////////////////////
	// Present
	//////////////////////////////////////////////////
	void swap(bool blitVirtualDisplay)
	{
		surface_t* disp = display_get();
		rdpq_attach_clear(disp, nullptr);

		if (blitVirtualDisplay && s_ci8Buffer)
		{
			// The framebuffer was written by the CPU (write-back cached); flush it to RDRAM
			// so the RDP blit reads the freshly rendered pixels instead of stale memory.
			data_cache_hit_writeback_invalidate((void*)s_ci8Buffer, (u32)s_vdispWidth * (u32)s_vdispHeight);

			surface_t fb = surface_make_linear((void*)s_ci8Buffer, FMT_CI8, (u16)s_vdispWidth, (u16)s_vdispHeight);

			rdpq_set_mode_standard();
			rdpq_mode_tlut(TLUT_RGBA16);
			rdpq_tex_upload_tlut(s_tlut, 0, 256);

			// Letterbox: center the 320x200 image in the (e.g.) 320x240 display.
			s32 xoff = ((s32)s_displayWidth  - (s32)s_vdispWidth)  / 2;
			s32 yoff = ((s32)s_displayHeight - (s32)s_vdispHeight) / 2;
			if (xoff < 0) { xoff = 0; }
			if (yoff < 0) { yoff = 0; }

			rdpq_tex_blit(&fb, (float)xoff, (float)yoff, nullptr);
		}

		rdpq_detach_show();

		// The RDP reads s_ci8Buffer asynchronously, but the caller clears + redraws that
		// same buffer next frame. Wait for the RDP to finish so the next memset/render does
		// not race the blit (which otherwise flickers the top of the image to black).
		rspq_wait();
	}

	//////////////////////////////////////////////////
	// Queries
	//////////////////////////////////////////////////
	u32  getVirtualDisplayWidth2D()  { return s_vdispWidth;  }
	u32  getVirtualDisplayWidth3D()  { return s_vdispWidth;  }
	u32  getVirtualDisplayHeight()   { return s_vdispHeight; }
	u32  getVirtualDisplayOffset2D() { return 0; }
	u32  getVirtualDisplayOffset3D() { return 0; }
	bool getWidescreen()             { return false; }
	bool getFrameBufferAsync()       { return false; }
	bool getGPUColorConvert()        { return true;  }
}
