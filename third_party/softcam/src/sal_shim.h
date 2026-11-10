#pragma once
#include <sal.h>
#define SHIM(x) 
#undef __deref_in
#define __deref_in
#undef __deref_inout_opt
#define __deref_inout_opt
#undef __deref_out
#define __deref_out
#undef __deref_out_opt
#define __deref_out_opt
#undef __deref_out_range
#define __deref_out_range(a,b)
#undef __field_ecount_opt
#define __field_ecount_opt(x)
#undef __format_string
#define __format_string
#undef __in
#define __in
#undef __in_bcount
#define __in_bcount(x)
#undef __in_bcount_opt
#define __in_bcount_opt(x)
#undef __in_ecount
#define __in_ecount(x)
#undef __in_opt
#define __in_opt
#undef __inout
#define __inout
#undef __inout_ecount_full
#define __inout_ecount_full(x)
#undef __inout_opt
#define __inout_opt
#undef __out
#define __out
#undef __out_bcount
#define __out_bcount(x)
#undef __out_ecount
#define __out_ecount(x)
#undef __out_ecount_part
#define __out_ecount_part(a,b)
#undef __out_opt
#define __out_opt
#undef __out_range
#define __out_range(a,b)
#undef __success
#define __success(x)
#ifndef AM_NOVTABLE
#define AM_NOVTABLE __declspec(novtable)
#endif
#undef __control_entrypoint
#define __control_entrypoint(x)
#ifdef __cplusplus
#include <windows.h>
#include <algorithm>
using std::min; using std::max;
#endif
#ifndef WIDTHBYTES
#define WIDTHBYTES(bits) ((DWORD)(((bits)+31) & (~31)) / 8)
#endif
#ifndef DIBWIDTHBYTES
#define DIBWIDTHBYTES(bi) (DWORD)WIDTHBYTES((DWORD)(bi).biWidth * (DWORD)(bi).biBitCount)
#define _DIBSIZE(bi) (DIBWIDTHBYTES(bi) * (DWORD)(bi).biHeight)
#define DIBSIZE(bi) ((bi).biHeight < 0 ? (-1)*(_DIBSIZE(bi)) : _DIBSIZE(bi))
#endif
#ifndef SIZE_PREHEADER
#define SIZE_PREHEADER (FIELD_OFFSET(VIDEOINFOHEADER,bmiHeader))
#endif
#ifndef HEADER
#define HEADER(pVideoInfo) (&(((VIDEOINFOHEADER *) (pVideoInfo))->bmiHeader))
#endif
#ifndef SIZE_MASKS
#define iPALETTE_COLORS 256
#define iEGA_COLORS 16
#define iMASK_COLORS 3
#define iTRUECOLOR 16
#define iRED 0
#define iGREEN 1
#define iBLUE 2
#define iPALETTE 8
#define iMAXBITS 8
#define SIZE_EGA_PALETTE (iEGA_COLORS * sizeof(RGBQUAD))
#define SIZE_PALETTE (iPALETTE_COLORS * sizeof(RGBQUAD))
#define SIZE_MASKS (iMASK_COLORS * sizeof(DWORD))
#define TRUECOLOR(pbmi) ((TRUECOLORINFO *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define COLORS(pbmi) ((RGBQUAD *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define BITMASKS(pbmi) ((DWORD *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define PALETTISED(pbmi) ((pbmi)->bmiHeader.biBitCount <= iPALETTE)
#define PALETTE_ENTRIES(pbmi) ((DWORD) 1 << (pbmi)->bmiHeader.biBitCount)
#endif
#ifndef SIZE_VIDEOHEADER
#define SIZE_VIDEOHEADER (sizeof(BITMAPINFOHEADER) + SIZE_PREHEADER)
#endif
