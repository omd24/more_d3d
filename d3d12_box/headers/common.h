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
#include <directxmath.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#define SIMPLE_ASSERT(exp, msg)  \
    if(!(exp)) {            \
        ::printf("[ERROR] %s() failed at line %d. \n" #msg "\n",  __FUNCTION__, __LINE__);    \
        abort();            \
    }

