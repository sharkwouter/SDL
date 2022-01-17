/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
  claim that you wrote the original software. If you use this software
  in a product, an acknowledgment in the product documentation would be
  appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
  misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_PSP

#include "SDL_hints.h"
#include "../SDL_sysrender.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pspge.h>
#include <stdarg.h>
#include <stdlib.h>
#include <vram.h>




/* PSP renderer implementation, based on the PGE  */

#define PSP_SCREEN_WIDTH    480
#define PSP_SCREEN_HEIGHT   272

#define PSP_FRAME_BUFFER_WIDTH  512
#define PSP_FRAME_BUFFER_SIZE   (PSP_FRAME_BUFFER_WIDTH*PSP_SCREEN_HEIGHT)

static unsigned int __attribute__((aligned(16))) DisplayList[262144];

/** Currently recorded epoch, resources used in current calls should be tagged with this epoch and will be considered in-flight*/
static unsigned long int recording_epoch = 1;

/** Signaled epoch, every resource tagged with lower or equal epoch are no more in-flight */
static volatile unsigned long int signaled_epoch = 0;

static SDL_cond* signaled_cond = NULL;
static SDL_mutex* signaled_mutex = NULL;
/**
 * Callback inserted in the display list to keep track of what the GPU is already done with
 *
 * Increase monotonically the epoch id
 */
void PSP_epoch_signal_callback(int cmd){
    //SDL_Log("Signaled : epoch"); // DO NOT LOG IN INTERRUPT HANDLERS...
    if(cmd == 1) {
        /* SDL_LockMutex(signaled_mutex); */
        signaled_epoch++;
        /* SDL_CondBroadcast(signaled_cond); */
        /* SDL_UnlockMutex(signaled_mutex); */
    }
}

static void sendDrawSyncSignal(int argument){
    int signal = 0x08;

    // Mandatory CMD order : SIG END FIN END
    sceGuSendCommandi(14,((signal & 0xff) << 16) | (argument & 0xffff));
    sceGuSendCommandi(12,0);
    sceGuSendCommandi(15,0);
    sceGuSendCommandi(12,0);
}

static void PSP_IncreaseEpoch(){
    // Record a signal DRAW_SYNC
    sendDrawSyncSignal(1); // Waits for drawing to be done
    sceGuSignal(GU_BEHAVIOR_CONTINUE, 1); // Signal by interrupt
    recording_epoch++;
}

static void PSP_WaitForEpoch(unsigned long epoch) {
    //SDL_Log("Waiting for epoch %ld, signaled : %ld, recording : %ld", epoch, signaled_epoch, recording_epoch);
    if(signaled_epoch >= epoch) {
        return;
    }

    while(epoch >= recording_epoch) {
        //We asked to wait in the middle of an epoch, slip new epoch to unlock us
        PSP_IncreaseEpoch();
    }

    /* SDL_LockMutex(signaled_mutex); */
    while(signaled_epoch < epoch) {
        /* SDL_CondWait(signaled_cond, signaled_mutex); */
    }
    /* SDL_UnlockMutex(signaled_mutex); */

    return;
}

#define COL5650(r,g,b,a)    ((r>>3) | ((g>>2)<<5) | ((b>>3)<<11))
#define COL5551(r,g,b,a)    ((r>>3) | ((g>>3)<<5) | ((b>>3)<<10) | (a>0?0x7000:0))
#define COL4444(r,g,b,a)    ((r>>4) | ((g>>4)<<4) | ((b>>4)<<8) | ((a>>4)<<12))
#define COL8888(r,g,b,a)    ((r) | ((g)<<8) | ((b)<<16) | ((a)<<24))

/**
 * Holds psp specific texture data
 *
 * Part of a hot-list of textures that are used as render targets
 * When short of vram we spill Least-Recently-Used render targets to system memory
 */
typedef struct PSP_TextureData
{
    void                *data;                              /**< Image data. */
    unsigned int        size;                               /**< Size of data in bytes. */
    unsigned int        width;                              /**< Image width. */
    unsigned int        height;                             /**< Image height. */
    unsigned int        textureWidth;                       /**< Texture width (power of two). */
    unsigned int        textureHeight;                      /**< Texture height (power of two). */
    unsigned int        bits;                               /**< Image bits per pixel. */
    unsigned int        format;                             /**< Image format - one of ::pgePixelFormat. */
    unsigned int        pitch;                              /**< Pitch in bytes from one row to another*/
    unsigned int        rows;                               /**< Number of rows actually allocated in memory*/
    SDL_bool            swizzled;                           /**< Is image swizzled. */
    unsigned long       epochUsed;                          /**< Last epoch this image was involved in*/
    struct PSP_TextureData*    prev;                        /**< Previous texture in list */
    struct PSP_TextureData*    next;                        /**< Next texture in list */
} PSP_TextureData;

static void PSP_UpdateEpoch(PSP_TextureData* data){
    data->epochUsed = recording_epoch;
}

static void PSP_UpdateEpochI(SDL_Texture* tex) {
    PSP_UpdateEpoch(tex->driverdata);
}

static int
PSP_TextureIsInFlight(PSP_TextureData* data){
    return data->epochUsed > signaled_epoch;
}

typedef struct
{
    SDL_BlendMode mode;
    unsigned int color;
    int shadeModel;
    SDL_Texture* texture;
} PSP_BlendState;

typedef struct
{
    PSP_TextureData* head;
    PSP_TextureData* tail;
} PSP_TextureList;

typedef struct
{
    void*              frontbuffer;                         /**< main screen buffer */
    void*              backbuffer;                          /**< buffer presented to display */
    SDL_Texture*       boundTarget;                         /**< currently bound rendertarget */
    SDL_bool           initialized;                         /**< is driver initialized */
    SDL_bool           displayListAvail;                    /**< is the display list already initialized for this frame */
    unsigned int       psm;                                 /**< format of the display buffers */
    unsigned int       bpp;                                 /**< bits per pixel of the main display */

    SDL_bool           vsync;                               /**< wether we do vsync */
    PSP_BlendState     blendState;                          /**< current blend mode */

    PSP_TextureList    lru_targets;                         /**< render target LRU */
    PSP_TextureList    gc_list;                             /**< garbage collection list */

    SDL_bool           vblank_not_reached;                  /**< wether vblank wasn't reached */
} PSP_RenderData;


typedef struct
{
    float   x, y, z;
} VertV;


typedef struct
{
    float   u, v;
    float   x, y, z;
} VertTV;

typedef struct
{
    SDL_Color col;
    float     x, y, z;
} VertCV;


typedef struct
{
    float     u, v;
    SDL_Color col;
    float     x, y, z;
} VertTCV;

#define PI   3.14159265358979f

#define radToDeg(x) ((x)*180.f/PI)
#define degToRad(x) ((x)*PI/180.f)

static float
MathAbs(float x)
{
    float result;

    __asm__ volatile (
                      "mtv      %1, S000\n"
                      "vabs.s   S000, S000\n"
                      "mfv      %0, S000\n"
                      : "=r"(result) : "r"(x));

    return result;
}

static void
MathSincos(float r, float *s, float *c)
{
    __asm__ volatile (
                      "mtv      %2, S002\n"
                      "vcst.s   S003, VFPU_2_PI\n"
                      "vmul.s   S002, S002, S003\n"
                      "vrot.p   C000, S002, [s, c]\n"
                      "mfv      %0, S000\n"
                      "mfv      %1, S001\n"
                      : "=r"(*s), "=r"(*c): "r"(r));
}

static void
Swap(float *a, float *b)
{
    float n=*a;
    *a = *b;
    *b = n;
}

static inline int
InVram(void* data)
{
    return data < (void*)0x04200000;
}

/* Return next power of 2 */
static int
TextureNextPow2(unsigned int w)
{
    unsigned int n = 2;
    if(w == 0)
        return 0;

    while(w > n)
        n <<= 1;

    return n;
}

static void psp_on_vblank(u32 sub, PSP_RenderData *data)
{
    if (data)
        data->vblank_not_reached = SDL_FALSE;
}


static int
PixelFormatToPSPFMT(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_BGR565:
        return GU_PSM_5650;
    case SDL_PIXELFORMAT_ABGR1555:
        return GU_PSM_5551;
    case SDL_PIXELFORMAT_ABGR4444:
        return GU_PSM_4444;
    case SDL_PIXELFORMAT_ABGR8888:
        return GU_PSM_8888;
    case SDL_PIXELFORMAT_INDEX8:
        return GU_PSM_T8;
    default:
        return GU_PSM_8888;
    }
}

///SECTION render target LRU management
static void
ListNodeRelink(PSP_TextureData* psp_texture) {
    if(psp_texture->prev) {
        psp_texture->prev->next = psp_texture->next;
    }
    if(psp_texture->next) {
        psp_texture->next->prev = psp_texture->prev;
    }
}

static void
ListNodePushFront(PSP_TextureList* list, PSP_TextureData* psp_texture) {
    psp_texture->next = list->head;
    if(list->head) {
        list->head->prev = psp_texture;
    }
    list->head = psp_texture;
    if(!list->tail) {
        list->tail = psp_texture;
    }
}

static void
ListNodeRemove(PSP_TextureList* list, PSP_TextureData* psp_texture) {
    ListNodeRelink(psp_texture);
    if(list->head == psp_texture) {
        list->head = psp_texture->next;
    }
    if(list->tail == psp_texture) {
        list->tail = psp_texture->prev;
    }
    psp_texture->prev = NULL;
    psp_texture->next = NULL;
}

static void
ListNodeBringFront(PSP_TextureList* list, PSP_TextureData* psp_texture) {
    if(list->head == psp_texture) {
        return; //nothing to do
    }
    ListNodeRemove(list, psp_texture);
    ListNodePushFront(list, psp_texture);
}

static void
TextureStorageFree(void* storage, size_t size) {
    if(InVram(storage)) {
        vfree(storage);
    } else {
        SDL_free(storage);
    }
}

static int
TextureSwizzle(PSP_TextureData *psp_texture, void* dst)
{
    int bytewidth, height;
    int rowblocks, rowblocksadd;
    int i, j;
    unsigned int blockaddress = 0;
    unsigned int *src = NULL;
    unsigned char *data = NULL;

    if(psp_texture->swizzled)
        return 1;

    bytewidth = psp_texture->pitch;
    height = psp_texture->rows;

    rowblocks = (bytewidth>>4);
    rowblocksadd = (rowblocks-1)<<7;

    src = (unsigned int*) psp_texture->data;
    //sceKernelDcacheInvalidateRange(src, psp_texture->size);

    data = dst;
    if(!data) {
        data = SDL_malloc(psp_texture->size);
    }

    if(!data) {
        return SDL_OutOfMemory();
    }

    for(j = 0; j < height; j++, blockaddress += 16)
        {
            unsigned int *block;

            block = (unsigned int*)&data[blockaddress];

            for(i = 0; i < rowblocks; i++)
                {
                    *block++ = *src++;
                    *block++ = *src++;
                    *block++ = *src++;
                    *block++ = *src++;
                    block += 28;
                }

            if((j & 0x7) == 0x7)
                blockaddress += rowblocksadd;
        }

    TextureStorageFree(psp_texture->data, psp_texture->size);
    psp_texture->data = data;
    psp_texture->swizzled = SDL_TRUE;

    sceKernelDcacheWritebackRange(psp_texture->data, psp_texture->size);
    return 1;
}

static int
TextureUnswizzle(PSP_TextureData *psp_texture, void* dst)
{
    int bytewidth, height;
    int widthblocks, heightblocks;
    int dstpitch, dstrow;
    int blockx, blocky;
    int j;
    unsigned int *src = NULL;
    unsigned char *data = NULL;
    unsigned char *ydst = NULL;

    if(!psp_texture->swizzled)
        return 1;

    //SDL_Log("Unswizzling %p", psp_texture);
    bytewidth = psp_texture->pitch;
    height = psp_texture->rows;

    widthblocks = bytewidth/16;
    heightblocks = height/8;

    dstpitch = (bytewidth - 16)/4;
    dstrow = bytewidth * 8;

    src = (unsigned int*) psp_texture->data;
    data = dst;

    if(!data) {
        data = SDL_malloc(psp_texture->size);
    }

    if(!data)
        return SDL_OutOfMemory();

    ydst = (unsigned char *)data;

    for(blocky = 0; blocky < heightblocks; ++blocky)
        {
            unsigned char *xdst = ydst;

            for(blockx = 0; blockx < widthblocks; ++blockx)
                {
                    unsigned int *block;

                    block = (unsigned int*)xdst;

                    for(j = 0; j < 8; ++j)
                        {
                            *(block++) = *(src++);
                            *(block++) = *(src++);
                            *(block++) = *(src++);
                            *(block++) = *(src++);
                            block += dstpitch;
                        }

                    xdst += 16;
                }

            ydst += dstrow;
        }

    TextureStorageFree(psp_texture->data, psp_texture->size);

    psp_texture->data = data;

    psp_texture->swizzled = SDL_FALSE;

    sceKernelDcacheWritebackRange(psp_texture->data, psp_texture->size);
    return 1;
}

static int
TextureSpillToSram(PSP_RenderData* data, PSP_TextureData* psp_texture)
{
    PSP_WaitForEpoch(psp_texture->epochUsed);
    // Assumes the texture is in VRAM
    if(psp_texture->swizzled) {
        //Texture was swizzled in vram, just copy to system memory
        void* data = SDL_malloc(psp_texture->size);
        if(!data) {
            return SDL_OutOfMemory();
        }

        SDL_memcpy(data, psp_texture->data, psp_texture->size);
        vfree(psp_texture->data);
        psp_texture->data = data;
        return 0;
    } else {
        return TextureSwizzle(psp_texture, NULL); //Will realloc in sysram
    }
}

static int
TexturePromoteToVram(PSP_RenderData* data, PSP_TextureData* psp_texture, SDL_bool target)
{
    // Assumes texture in sram and a large enough continuous block in vram
    void* tdata = valloc(psp_texture->size);
    PSP_WaitForEpoch(psp_texture->epochUsed);
    if(psp_texture->swizzled && target) {
        return TextureUnswizzle(psp_texture, tdata);
    } else {
        SDL_memcpy(tdata, psp_texture->data, psp_texture->size);
        SDL_free(psp_texture->data);
        psp_texture->data = tdata;
        return 0;
    }
}

static int
TextureSpillLRU(PSP_RenderData* data, size_t wanted) {
    PSP_TextureData* lru = data->lru_targets.tail;
    if(lru) {
        if(TextureSpillToSram(data, lru) < 0) {
            return -1;
        }
        ListNodeRemove(&data->lru_targets, lru);
    } else {
        // Asked to spill but there nothing to spill
        return SDL_SetError("Could not spill more VRAM to system memory. VRAM : %dKB,(%dKB), wanted %dKB", vmemavail()/1024, vlargestblock()/1024, wanted/1024);
    }
    return 0;
}

static int
TextureSpillTargetsForSpace(PSP_RenderData* data, size_t size)
{
    while(vlargestblock() < size) {
        if(TextureSpillLRU(data, size) < 0) {
            return -1;
        }
    }
    return 0;
}

static int
TextureBindAsTarget(PSP_RenderData* data, PSP_TextureData* psp_texture) {
    unsigned int dstFormat;

    if(!InVram(psp_texture->data)) {
        // Bring back the texture in vram
        if(TextureSpillTargetsForSpace(data, psp_texture->size) < 0) {
            return -1;
        }
        if(TexturePromoteToVram(data, psp_texture, SDL_TRUE) < 0) {
            return -1;
        }
    }
    ListNodeBringFront(&data->lru_targets, psp_texture);
    sceGuDrawBufferList(psp_texture->format, vrelptr(psp_texture->data), (psp_texture->pitch*8)/psp_texture->bits);

    // Stencil alpha dst hack
    dstFormat = psp_texture->format;
    if(dstFormat == GU_PSM_5551) {
        sceGuEnable(GU_STENCIL_TEST);
        sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
        sceGuStencilFunc(GU_GEQUAL, 0xff, 0xff);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x00, 0xff);
    } else {
        sceGuDisable(GU_STENCIL_TEST);
        sceGuDisable(GU_ALPHA_TEST);
    }

    //Enable scissor to avoid drawing outside viewport
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuScissor(0,0,psp_texture->width, psp_texture->height);
    return 0;
}

static void
PSP_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{
}


static int
PSP_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    PSP_RenderData *data = renderer->driverdata;
    PSP_TextureData* psp_texture = (PSP_TextureData*) SDL_calloc(1, sizeof(*psp_texture));

    if(!psp_texture)
        return SDL_OutOfMemory();

    psp_texture->swizzled = SDL_FALSE;
    psp_texture->width = texture->w;
    psp_texture->height = texture->h;
    psp_texture->textureHeight = TextureNextPow2(texture->h);
    psp_texture->textureWidth = TextureNextPow2(texture->w);
    psp_texture->format = PixelFormatToPSPFMT(texture->format);
    psp_texture->epochUsed = 0;

    switch(psp_texture->format)
        {
        case GU_PSM_T8:
            psp_texture->bits = 8;
            break;
        case GU_PSM_5650:
        case GU_PSM_5551:
        case GU_PSM_4444:
            psp_texture->bits = 16;
            break;

        case GU_PSM_8888:
            psp_texture->bits = 32;
            break;

        default:
            return -1;
        }

    psp_texture->pitch = psp_texture->textureWidth * SDL_BYTESPERPIXEL(texture->format);

    //Round the pitch up to 16 bytes
    psp_texture->pitch = (psp_texture->pitch + 15) & 0xfffffff0;
    //Round the rows up to 8px
    psp_texture->rows = (psp_texture->height + 7) & 0xfffffff8;

    psp_texture->size = psp_texture->rows*psp_texture->pitch;
    if(texture->access & SDL_TEXTUREACCESS_TARGET) {
        if(TextureSpillTargetsForSpace(renderer->driverdata, psp_texture->size) < 0){
            return -1;
        }
        psp_texture->data = valloc(psp_texture->size);
        if(psp_texture->data) {
            ListNodePushFront(&data->lru_targets, psp_texture);
        }
    } else {
        psp_texture->data = SDL_calloc(1, psp_texture->size);
    }

    if(!psp_texture->data)
        {
            SDL_free(psp_texture);
            return SDL_OutOfMemory();
        }
    texture->driverdata = psp_texture;

    return 0;
}

static int
TextureShouldSwizzle(PSP_TextureData* psp_texture, SDL_Texture *texture)
{
    return !((texture->access == SDL_TEXTUREACCESS_TARGET) && InVram(psp_texture->data))
        && (texture->w >= 16 || texture->h >= 16);
}

static void
TextureActivate(SDL_Texture * texture)
{
    PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
    int scaleMode = (texture->scaleMode == SDL_ScaleModeNearest) ? GU_NEAREST : GU_LINEAR;

    /* Swizzling is useless with small textures. */
    if (TextureShouldSwizzle(psp_texture, texture))
        {
            TextureSwizzle(psp_texture, NULL);
        }

    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
    sceGuTexMode(psp_texture->format, 0, 0, psp_texture->swizzled);
    sceGuTexFilter(scaleMode, scaleMode); /* GU_NEAREST good for tile-map */
                                          /* GU_LINEAR good for scaling */
    sceGuTexImage(0, psp_texture->textureWidth, psp_texture->textureHeight, (psp_texture->pitch*8)/psp_texture->bits, psp_texture->data);
}

static int
PSP_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                const SDL_Rect * rect, void **pixels, int *pitch);

static void
PSP_UnlockTexture(SDL_Renderer* renderer, SDL_Texture* texture);

static int
PSP_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                  const SDL_Rect * rect, const void *pixels, int pitch)
{
    PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
    const Uint8 *src;
    Uint8 *dst;
    int row, length,dpitch;
    src = pixels;

    PSP_LockTexture(renderer, texture,rect,(void **)&dst, &dpitch);
    length = rect->w * SDL_BYTESPERPIXEL(texture->format);
    if (length == pitch && length == dpitch) {
        SDL_memcpy(dst, src, length*rect->h);
    } else {
        for (row = 0; row < rect->h; ++row) {
            SDL_memcpy(dst, src, length);
            src += pitch;
            dst += dpitch;
        }
    }

    psp_texture->swizzled = SDL_FALSE;

    //sceKernelDcacheWritebackRange(psp_texture->data, psp_texture->size);
    PSP_UnlockTexture(renderer, texture);
    return 0;
}

static int
PSP_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                const SDL_Rect * rect, void **pixels, int *pitch)
{
    PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;

    TextureUnswizzle(psp_texture, NULL);

    *pixels =
        (void *) ((Uint8 *) psp_texture->data + rect->y * psp_texture->pitch +
                  rect->x * SDL_BYTESPERPIXEL(texture->format));
    *pitch = psp_texture->pitch;
    return 0;
}

static void
PSP_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
    //SDL_Rect rect;

    /* We do whole texture updates, at least for now */
    //rect.x = 0;
    //rect.y = 0;
    //rect.w = texture->w;
    //rect.h = texture->h;
    //PSP_UpdateTexture(renderer, texture, &rect, psp_texture->data, psp_texture->pitch);
    // TODO this should stay no-op
    sceKernelDcacheWritebackRange(psp_texture->data, psp_texture->size);
}

static void
PSP_SetTextureScaleMode(SDL_Renderer * renderer, SDL_Texture * texture, SDL_ScaleMode scaleMode)
{
    /* Nothing to do because TextureActivate takes care of it */
}

static int
PSP_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture)
{
    return 0;
}

static int
PSP_QueueSetViewport(SDL_Renderer * renderer, SDL_RenderCommand *cmd)
{
    return 0;  /* nothing to do in this backend. */
}

static int
PSP_QueueDrawPoints(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FPoint * points, int count)
{
    VertCV *verts = (VertCV *) SDL_AllocateRenderVertices(renderer, count * sizeof (VertCV), 4, &cmd->data.draw.first);
    SDL_Color col = {cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a};
    int i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, points++) {
        verts->x = points->x;
        verts->y = points->y;
        verts->z = 0.0f;
        verts->col = col;
    }

    return 0;
}

static int
PSP_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                  const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
                  int num_vertices, const void *indices, int num_indices, int size_indices,
                  float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    if (texture == NULL) {
        VertCV *verts;
        verts = (VertCV *) SDL_AllocateRenderVertices(renderer, count * sizeof (VertCV), 4, &cmd->data.draw.first);
        if (!verts) {
            return -1;
        }

        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char*)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char*)color + j * color_stride);

            verts->x = xy_[0] * scale_x;
            verts->y = xy_[1] * scale_y;
            verts->z = 0;

            verts->col = col_;

            verts++;
        }
    } else {
        PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
        VertTCV *verts;
        verts = (VertTCV *) SDL_AllocateRenderVertices(renderer, count * sizeof (VertTCV), 4, &cmd->data.draw.first);
        if (!verts) {
            return -1;
        }

        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            float *uv_;

            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char*)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char*)color + j * color_stride);
            uv_ = (float *)((char*)uv + j * uv_stride);

            verts->x = xy_[0] * scale_x;
            verts->y = xy_[1] * scale_y;
            verts->z = 0;

            verts->col = col_;

            verts->u = uv_[0] * psp_texture->width;
            verts->v = uv_[1] * psp_texture->height;

            verts++;
        }
    }

    return 0;
}

static int
PSP_QueueFillRects(SDL_Renderer * renderer, SDL_RenderCommand *cmd, const SDL_FRect * rects, int count)
{
    VertCV *verts = (VertCV *) SDL_AllocateRenderVertices(renderer, count * 2 * sizeof (VertCV), 4, &cmd->data.draw.first);
    SDL_Color col = {cmd->data.draw.r, cmd->data.draw.g, cmd->data.draw.b, cmd->data.draw.a};
    int i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count * 2;
    for (i = 0; i < count; i++, rects++) {
        verts->x = rects->x;
        verts->y = rects->y;
        verts->z = 0.0f;
        verts->col = col;
        verts++;

        verts->x = rects->x + rects->w + 0.5f;
        verts->y = rects->y + rects->h + 0.5f;
        verts->z = 0.0f;
        verts->col = col;
        verts++;
    }

    return 0;
}

static int
PSP_QueueCopy(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
              const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
    VertTV *verts;
    const float x = dstrect->x;
    const float y = dstrect->y;
    const float width = dstrect->w;
    const float height = dstrect->h;

    const float u0 = srcrect->x;
    const float v0 = srcrect->y;
    const float u1 = srcrect->x + srcrect->w;
    const float v1 = srcrect->y + srcrect->h;

    if((MathAbs(u1) - MathAbs(u0)) < 64.0f)
        {
            verts = (VertTV *) SDL_AllocateRenderVertices(renderer, 2 * sizeof (VertTV), 4, &cmd->data.draw.first);
            if (!verts) {
                return -1;
            }

            cmd->data.draw.count = 2;

            verts->u = u0;
            verts->v = v0;
            verts->x = x;
            verts->y = y;
            verts->z = 0;
            verts++;

            verts->u = u1;
            verts->v = v1;
            verts->x = x + width;
            verts->y = y + height;
            verts->z = 0;
            verts++;
        }
    else
        {
            float start, end;
            float curU = u0;
            float curX = x;
            const float endX = x + width;
            const float slice = 64.0f;
            const size_t count = SDL_ceilf(width / slice);
            size_t i;
            float ustep = (u1 - u0)/width * slice;

            if(ustep < 0.0f)
                ustep = -ustep;

            cmd->data.draw.count = count*2;

            verts = (VertTV *) SDL_AllocateRenderVertices(renderer, count * 2 * sizeof (VertTV), 4, &cmd->data.draw.first);
            if (!verts) {
                return -1;
            }


            for(i = 0, start = 0, end = width; i < count; i++, start += slice)
                {
                    const float polyWidth = ((curX + slice) > endX) ? (endX - curX) : slice;
                    const float sourceWidth = ((curU + ustep) > u1) ? (u1 - curU) : ustep;

                    SDL_assert(start < end);

                    verts->u = curU;
                    verts->v = v0;
                    verts->x = curX;
                    verts->y = y;
                    verts->z = 0;
                    verts++;

                    curU += sourceWidth;
                    curX += polyWidth;

                    verts->u = curU;
                    verts->v = v1;
                    verts->x = curX;
                    verts->y = (y + height);
                    verts->z = 0;
                    verts++;
                }
        }

    return 0;
}

static int
PSP_QueueCopyEx(SDL_Renderer * renderer, SDL_RenderCommand *cmd, SDL_Texture * texture,
                const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip)
{
    VertTV *verts = (VertTV *) SDL_AllocateRenderVertices(renderer, 4 * sizeof (VertTV), 4, &cmd->data.draw.first);
    const float centerx = center->x;
    const float centery = center->y;
    const float x = dstrect->x + centerx;
    const float y = dstrect->y + centery;
    const float width = dstrect->w - centerx;
    const float height = dstrect->h - centery;
    float s, c;
    float cw1, sw1, ch1, sh1, cw2, sw2, ch2, sh2;

    float u0 = srcrect->x;
    float v0 = srcrect->y;
    float u1 = srcrect->x + srcrect->w;
    float v1 = srcrect->y + srcrect->h;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = 1;

    MathSincos(degToRad(360-angle), &s, &c);

    cw1 = c * -centerx;
    sw1 = s * -centerx;
    ch1 = c * -centery;
    sh1 = s * -centery;
    cw2 = c * width;
    sw2 = s * width;
    ch2 = c * height;
    sh2 = s * height;

    if (flip & SDL_FLIP_VERTICAL) {
        Swap(&v0, &v1);
    }

    if (flip & SDL_FLIP_HORIZONTAL) {
        Swap(&u0, &u1);
    }

    verts->u = u0;
    verts->v = v0;
    verts->x = x + cw1 + sh1;
    verts->y = y - sw1 + ch1;
    verts->z = 0;
    verts++;

    verts->u = u0;
    verts->v = v1;
    verts->x = x + cw1 + sh2;
    verts->y = y - sw1 + ch2;
    verts->z = 0;
    verts++;

    verts->u = u1;
    verts->v = v1;
    verts->x = x + cw2 + sh2;
    verts->y = y - sw2 + ch2;
    verts->z = 0;
    verts++;

    verts->u = u1;
    verts->v = v0;
    verts->x = x + cw2 + sh1;
    verts->y = y - sw2 + ch1;
    verts->z = 0;
    verts++;

    return 0;
}

static void
ResetBlendState(PSP_BlendState* state) {
    sceGuColor(0xffffffff);
    state->color = 0xffffffff;
    state->mode = SDL_BLENDMODE_INVALID;
    state->texture = NULL;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    state->shadeModel = GU_SMOOTH;
}

static void
StartDrawing(SDL_Renderer * renderer)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    ScePspFMatrix4 id;

    // Check if we need to start GU displaylist
    if(!data->displayListAvail) {
        sceGuStart(GU_DIRECT, DisplayList);
        data->displayListAvail = SDL_TRUE;
        // ResetBlendState(&data->blendState);
    }

    // Check if we need a draw buffer change
    if(renderer->target != data->boundTarget) {
        SDL_Texture* texture = renderer->target;
        if(texture) {
            PSP_TextureData* psp_texture = (PSP_TextureData*) texture->driverdata;
            PSP_UpdateEpoch(psp_texture);
            // Set target, registering LRU
            TextureBindAsTarget(data, psp_texture);
        } else {
            // Set target back to screen
            sceGuDrawBufferList(data->psm, vrelptr(data->frontbuffer), PSP_FRAME_BUFFER_WIDTH);
        }
        data->boundTarget = texture;
    }
}


static void
PSP_SetBlendState(PSP_RenderData* data, PSP_BlendState* state)
{
    PSP_BlendState* current = &data->blendState;

    if (state->mode != current->mode) {
        switch (state->mode) {
        case SDL_BLENDMODE_NONE:
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
            sceGuDisable(GU_BLEND);
            break;
        case SDL_BLENDMODE_BLEND:
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
            sceGuEnable(GU_BLEND);
            break;
        case SDL_BLENDMODE_ADD:
            sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF );
            sceGuEnable(GU_BLEND);
            break;
        case SDL_BLENDMODE_MOD:
            sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_SRC_COLOR, 0, 0);
            sceGuEnable(GU_BLEND);
            break;
        case SDL_BLENDMODE_MUL:
            sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
            sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            sceGuEnable(GU_BLEND);
            break;
        case SDL_BLENDMODE_INVALID:
            break;
        }
    }

    if(state->color != current->color) {
        sceGuColor(state->color);
    }

    if(state->shadeModel != current->shadeModel) {
        sceGuShadeModel(state->shadeModel);
    }

    if(state->texture != current->texture) {
        if(state->texture != NULL) {
            TextureActivate(state->texture);
            sceGuEnable(GU_TEXTURE_2D);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
    }


    *current = *state;
}

static PSP_BlendState
PSP_CmdBlendState(const SDL_RenderCommand* cmd){
    const Uint8 r = cmd->data.draw.r;
    const Uint8 g = cmd->data.draw.g;
    const Uint8 b = cmd->data.draw.b;
    const Uint8 a = cmd->data.draw.a;
    PSP_BlendState state = {
        .color = GU_RGBA(r,g,b,a),
        .texture = cmd->data.draw.texture,
        .mode = cmd->data.draw.blend,
        .shadeModel = GU_SMOOTH
    };
    return state;
}

static void
PSP_SetBlendStateFromCmd(PSP_RenderData* data, const SDL_RenderCommand* cmd){
    PSP_BlendState state = PSP_CmdBlendState(cmd);
    PSP_SetBlendState(data, &state);
}

typedef struct{
    PSP_BlendState bstate;
    int primitiveType;
    int vertexShape;
} PSP_DrawState;

static PSP_DrawState
PSP_CmdDrawState(const SDL_RenderCommand* cmd)
{
    int primitiveType = 0;
    int vertexShape = 0;

    int transform = GU_TRANSFORM_2D;
    switch(cmd->command){
    case SDL_RENDERCMD_DRAW_LINES:
        primitiveType = GU_LINE_STRIP;
        vertexShape = GU_COLOR_8888 | GU_VERTEX_32BITF | transform;
        break;
    case SDL_RENDERCMD_DRAW_POINTS:
        primitiveType = GU_POINTS;
        vertexShape = GU_COLOR_8888 | GU_VERTEX_32BITF | transform;
        break;
    case SDL_RENDERCMD_FILL_RECTS:
        primitiveType = GU_SPRITES;
        vertexShape = GU_COLOR_8888 | GU_VERTEX_32BITF | transform;
        break;
    case SDL_RENDERCMD_COPY:
        primitiveType = GU_SPRITES;
        vertexShape = GU_VERTEX_32BITF | transform;
        break;
    case SDL_RENDERCMD_GEOMETRY:
    case SDL_RENDERCMD_COPY_EX:
        primitiveType = GU_TRIANGLES;
        vertexShape = GU_COLOR_8888 | GU_VERTEX_32BITF | transform;
        break;
    default:
        primitiveType = -1;
        vertexShape = -1;
    }
    // If draw has texture we also need to have uv coordinates
    if(cmd->data.draw.texture){
        vertexShape |= GU_TEXTURE_32BITF;
    }
    {
        PSP_DrawState ds = {
            .bstate = PSP_CmdBlendState(cmd),
            .primitiveType = primitiveType,
            .vertexShape = vertexShape
        };
        return ds;
    }
}

static int
PSP_BatchableStates(const PSP_DrawState* a, const PSP_DrawState* b){
    return a->primitiveType != GU_LINE_STRIP && memcmp(a,b, sizeof(PSP_DrawState)) == 0;
}

static int
PSP_RunCommandQueue(SDL_Renderer * renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    Uint8 *gpumem = NULL;
    PSP_DrawState resetDs = {
        .bstate = {
            .color = -1,
            .texture = NULL,
            .mode = SDL_BLENDMODE_INVALID,
            .shadeModel = -1
        },
        .primitiveType = -1,
        .vertexShape = -1
    };
    PSP_DrawState currDs = resetDs;
    size_t vCount = 0; //Number of batched vertices
    void* verts = NULL;

    void flush() {
        if(vCount && verts) {
            PSP_SetBlendState(data, &currDs.bstate);
            sceGuDrawArray(currDs.primitiveType, currDs.vertexShape, vCount, NULL, verts);
            if(currDs.bstate.texture){
                PSP_UpdateEpochI(currDs.bstate.texture);
            }
        }
        vCount = 0;
        verts = NULL;
    }

    StartDrawing(renderer);

    /* note that before the renderer interface change, this would do extrememly small
       batches with sceGuGetMemory()--a few vertices at a time--and it's not clear that
       this won't fail if you try to push 100,000 draw calls in a single batch.
       I don't know what the limits on PSP hardware are. It might be useful to have
       rendering backends report a reasonable maximum, so the higher level can flush
       if we appear to be exceeding that. */
    gpumem = (Uint8 *) sceGuGetMemory(vertsize);
    if (!gpumem) {
        return SDL_SetError("Couldn't obtain a %d-byte vertex buffer!", (int) vertsize);
    }
    SDL_memcpy(gpumem, vertices, vertsize);
    verts = gpumem;


    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR: {
            break;  /* !!! FIXME: we could cache drawstate like color */
        }

        case SDL_RENDERCMD_SETVIEWPORT: {
            SDL_Rect *viewport = &cmd->data.viewport.rect;
            //SDL_Log("Setting viewport (%d, %d, %d, %d)", viewport->x, viewport->y, viewport->w, viewport->h);
            sceGuOffset(
                        2048 - (viewport->w >> 1),
                        2048 - (viewport->h >> 1));
                        sceGuViewport(2048, 2048, viewport->w, viewport->h);
            sceGuEnable(GU_SCISSOR_TEST);
            sceGuScissor(viewport->x, viewport->y, viewport->w, viewport->h);
            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT: {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            if(cmd->data.cliprect.enabled){
                sceGuEnable(GU_SCISSOR_TEST);
                sceGuScissor(rect->x, rect->y, rect->w, rect->h);
            } else if(!data->boundTarget) {
                sceGuDisable(GU_SCISSOR_TEST);
            }
            break;
        }

        case SDL_RENDERCMD_CLEAR: {
            const Uint8 r = cmd->data.color.r;
            const Uint8 g = cmd->data.color.g;
            const Uint8 b = cmd->data.color.b;
            const Uint8 a = cmd->data.color.a;
            sceGuClearColor(GU_RGBA(r,g,b,a));
            sceGuClearStencil(a);
            sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_DRAW_LINES:
        case SDL_RENDERCMD_FILL_RECTS:
        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
        case SDL_RENDERCMD_GEOMETRY: {
            PSP_DrawState cmdDs = PSP_CmdDrawState(cmd);

            if(!PSP_BatchableStates(&currDs, &cmdDs)) {
                flush();
                currDs = cmdDs;
                verts = gpumem + cmd->data.draw.first;
                vCount = cmd->data.draw.count;
            } else {
                vCount += cmd->data.draw.count;
            }
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }

    //Do a last flush
    flush();

    // Tag the end of this epoch time slice
    PSP_IncreaseEpoch();
    return 0;
}

static int
PSP_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                     Uint32 pixel_format, void * pixels, int pitch)
{
    return SDL_Unsupported();
}

static void
PSP_GCFreeNotInFlight(PSP_RenderData* renderdata);

static void
PSP_RenderPresent(SDL_Renderer * renderer)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    if(!data->displayListAvail)
        return;

    data->displayListAvail = SDL_FALSE;
    sceGuFinish();
    sceGuSync(0,0);

    PSP_GCFreeNotInFlight(data);
    if ((data->vsync) && (data->vblank_not_reached))
        sceDisplayWaitVblankStart();
    data->vblank_not_reached = SDL_TRUE;

    data->backbuffer = data->frontbuffer;
    data->frontbuffer = vabsptr(sceGuSwapBuffers());

}

static void
PSP_DestroyImpl(PSP_RenderData* renderdata, PSP_TextureData* psp_texture)
{
    TextureStorageFree(psp_texture->data, psp_texture->size);
    SDL_free(psp_texture);
}

static void
PSP_GCFreeNotInFlight(PSP_RenderData* renderdata){
    PSP_TextureData* node = renderdata->gc_list.tail;
    PSP_TextureData* curr = NULL;
    //SDL_Log("======== GC free =========");
    while(node != NULL) {
        //SDL_Log("%p", node);
        curr = node;
        node = node->prev;
        if(!PSP_TextureIsInFlight(curr)) {
            ListNodeRemove(&renderdata->gc_list, curr);
            PSP_DestroyImpl(renderdata, curr);
        }
    }
}

static void
PSP_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
    PSP_RenderData *renderdata = (PSP_RenderData *) renderer->driverdata;
    PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;

    if (renderdata == 0)
        return;

    if(psp_texture == 0)
        return;

    ListNodeRemove(&renderdata->lru_targets, psp_texture);

    if(PSP_TextureIsInFlight(psp_texture)){
        // Texture is in use, schedule deletion of the implementation for later
        ListNodePushFront(&renderdata->gc_list, psp_texture);
    } else {
        // Not used anymore, destroy now
        PSP_DestroyImpl(renderdata, psp_texture);
    }

    //Do a GC pass to free older resources that are not anymore in flight
    PSP_GCFreeNotInFlight(renderdata);

    texture->driverdata = NULL;
    if(renderdata->boundTarget == texture){
        renderdata->boundTarget = NULL;
    }
    if(renderdata->blendState.texture == texture) {
        renderdata->blendState.texture = NULL;
    }
}

static void
PSP_DestroyRenderer(SDL_Renderer * renderer)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    if (data) {
        if (!data->initialized)
            return;

        StartDrawing(renderer);

        sceKernelDisableSubIntr(PSP_VBLANK_INT, 0);
        sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT,0);
        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_FALSE);
        sceGuTerm();
        vfree(data->backbuffer);
        vfree(data->frontbuffer);

        data->initialized = SDL_FALSE;
        data->displayListAvail = SDL_FALSE;
        SDL_free(data);
    }
    SDL_free(renderer);

    SDL_DestroyCond(signaled_cond);
    SDL_DestroyMutex(signaled_mutex);
}

static int
PSP_SetVSync(SDL_Renderer * renderer, const int vsync)
{
    PSP_RenderData *data = renderer->driverdata;
    data->vsync = vsync;
    return 0;
}

SDL_Renderer *
PSP_CreateRenderer(SDL_Window * window, Uint32 flags)
{

    SDL_Renderer *renderer;
    PSP_RenderData *data;
    int pixelformat;
    void* doublebuffer = NULL;

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (PSP_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        PSP_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }


    renderer->WindowEvent = PSP_WindowEvent;
    renderer->CreateTexture = PSP_CreateTexture;
    renderer->UpdateTexture = PSP_UpdateTexture;
    renderer->LockTexture = PSP_LockTexture;
    renderer->UnlockTexture = PSP_UnlockTexture;
    renderer->SetTextureScaleMode = PSP_SetTextureScaleMode;
    renderer->SetRenderTarget = PSP_SetRenderTarget;
    renderer->QueueSetViewport = PSP_QueueSetViewport;
    renderer->QueueSetDrawColor = PSP_QueueSetViewport;  /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = PSP_QueueDrawPoints;
    renderer->QueueDrawLines = PSP_QueueDrawPoints;  /* lines and points queue vertices the same way. */
    renderer->QueueGeometry = PSP_QueueGeometry;
    renderer->QueueFillRects = PSP_QueueFillRects;
    renderer->QueueCopy = PSP_QueueCopy;
    //renderer->QueueCopyEx = PSP_QueueCopyEx;
    renderer->RunCommandQueue = PSP_RunCommandQueue;
    renderer->RenderReadPixels = PSP_RenderReadPixels;
    renderer->RenderPresent = PSP_RenderPresent;
    renderer->DestroyTexture = PSP_DestroyTexture;
    renderer->DestroyRenderer = PSP_DestroyRenderer;
    renderer->SetVSync = PSP_SetVSync;
    renderer->info = PSP_RenderDriver.info;
    renderer->info.flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    renderer->driverdata = data;
    renderer->window = window;

    if (data->initialized != SDL_FALSE)
        return 0;
    data->initialized = SDL_TRUE;

    //data->lru_targets = NULL;
    //data->gc_list = NULL;

    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        data->vsync = SDL_TRUE;
    } else {
        data->vsync = SDL_FALSE;
    }

    pixelformat=PixelFormatToPSPFMT(SDL_GetWindowPixelFormat(window));
    switch(pixelformat)
        {
        case GU_PSM_4444:
        case GU_PSM_5650:
        case GU_PSM_5551:
            data->bpp = 2;
            data->psm = pixelformat;
            break;
        default:
            data->bpp = 4;
            data->psm = GU_PSM_8888;
            break;
        }

    doublebuffer = valloc(PSP_FRAME_BUFFER_SIZE*data->bpp*2);
    data->backbuffer = doublebuffer;
    data->frontbuffer = ((uint8_t*)doublebuffer)+PSP_FRAME_BUFFER_SIZE*data->bpp;

    sceGuInit();

    /* setup GU */
    sceGuStart(GU_DIRECT, DisplayList);
    sceGuDrawBuffer(data->psm, vrelptr(data->frontbuffer), PSP_FRAME_BUFFER_WIDTH);
    sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, vrelptr(data->backbuffer), PSP_FRAME_BUFFER_WIDTH);


    sceGuOffset(2048 - (PSP_SCREEN_WIDTH>>1), 2048 - (PSP_SCREEN_HEIGHT>>1));
    sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);


    sceGuDisable(GU_DEPTH_TEST);

    /* Scissoring */
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Backface culling */
    /*
      FIXME: Culling probably un-needed ? It can conflict with SDL_RENDERCMD_GEOMETRY
      sceGuFrontFace(GU_CCW);
      sceGuEnable(GU_CULL_FACE);
    */

    //Setup initial blend state
    ResetBlendState(&data->blendState);

    sceGuSetCallback(GU_CALLBACK_SIGNAL, PSP_epoch_signal_callback);
    sceGuFinish();
    sceGuSync(0,0);
    sceDisplayWaitVblankStartCB();
    sceGuDisplay(GU_TRUE);

    /* Improve performance when VSYC is enabled and it is not reaching the 60 FPS */
    data->vblank_not_reached = SDL_TRUE;
    sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, 0, psp_on_vblank, data);
    sceKernelEnableSubIntr(PSP_VBLANK_INT, 0);
    //sceGuCallMode(1);

    signaled_cond = SDL_CreateCond();
    if(signaled_cond == NULL){
        SDL_OutOfMemory();
        return NULL;
    }

    signaled_mutex = SDL_CreateMutex();
    if(signaled_mutex == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }


    return renderer;
}

SDL_RenderDriver PSP_RenderDriver = {
    .CreateRenderer = PSP_CreateRenderer,
    .info = {
        .name = "PSP",
        .flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
        .num_texture_formats = 4,
        .texture_formats = { [0] = SDL_PIXELFORMAT_BGR565,
            [1] = SDL_PIXELFORMAT_ABGR1555,
            [2] = SDL_PIXELFORMAT_ABGR4444,
            [3] = SDL_PIXELFORMAT_ABGR8888,
        },
        .max_texture_width = 512,
        .max_texture_height = 512,
    }
};

#endif /* SDL_VIDEO_RENDER_PSP */

/* vi: set ts=4 sw=4 expandtab: */

