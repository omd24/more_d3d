/* ===========================================================
   #File: common.h #
   #Date: 17 Jan 2021 #
   #Revision: 1.0 #
   #Creator: Omid Miresmaeili #
   #Description: Common stuff #
   #Notice: (C) Copyright 2021 by Omid. All Rights Reserved. #
   =========================================================== */
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <d3dcommon.h>
#include <directxmath.h>
#include <windowsx.h>
#include <tchar.h>

#include <DirectXColors.h>
#include <DirectXCollision.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#define SIMPLE_ASSERT(exp, msg)  \
    if(!(exp)) {            \
        ::printf("[ERROR] %s() failed at line %d. \n" #msg "\n",  __FUNCTION__, __LINE__);    \
        abort();            \
    }

#define SIMPLE_ASSERT_FALSE(exp, msg)   SIMPLE_ASSERT(!exp, msg)

#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
//#define SUCCEEDED_OPERATION(hr)   (((HRESULT)(hr)) == S_OK)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
//#define FAILED_OPERATION(hr)      (((HRESULT)(hr)) != S_OK)
#define CHECK_AND_FAIL(hr)                          \
    if (FAILED(hr)) {                               \
        ::printf("[ERROR] " #hr "() failed at line %d. \n", __LINE__);   \
        ::abort();                                  \
    }                                               \

