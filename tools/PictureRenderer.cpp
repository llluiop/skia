/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "PictureRenderer.h"
#include "picture_utils.h"
#include "SamplePipeControllers.h"
#include "SkBitmapHasher.h"
#include "SkCanvas.h"
#include "SkData.h"
#include "SkDevice.h"
#include "SkDiscardableMemoryPool.h"
#include "SkGPipe.h"
#if SK_SUPPORT_GPU
#include "gl/GrGLDefines.h"
#include "SkGpuDevice.h"
#endif
#include "SkGraphics.h"
#include "SkImageEncoder.h"
#include "SkMaskFilter.h"
#include "SkMatrix.h"
#include "SkOSFile.h"
#include "SkPicture.h"
#include "SkPictureUtils.h"
#include "SkPixelRef.h"
#include "SkQuadTree.h"
#include "SkQuadTreePicture.h"
#include "SkRTree.h"
#include "SkScalar.h"
#include "SkStream.h"
#include "SkString.h"
#include "SkTemplates.h"
#include "SkTileGridPicture.h"
#include "SkTDArray.h"
#include "SkThreadUtils.h"
#include "SkTypes.h"

static inline SkScalar scalar_log2(SkScalar x) {
    static const SkScalar log2_conversion_factor = SkScalarDiv(1, SkScalarLog(2));

    return SkScalarLog(x) * log2_conversion_factor;
}

namespace sk_tools {

enum {
    kDefaultTileWidth = 256,
    kDefaultTileHeight = 256
};

/*
 * TODO(epoger): Make constant strings consistent instead of mixing hypenated and camel-caps.
 *
 * TODO(epoger): Similar constants are already maintained in 2 other places:
 * gm/gm_json.py and gm/gm_expectations.cpp. We shouldn't add yet a third place.
 * Figure out a way to share the definitions instead.
 *
 * Note that, as of https://codereview.chromium.org/226293002 , the JSON
 * schema used here has started to differ from the one in gm_expectations.cpp .
 * TODO(epoger): Consider getting GM and render_pictures to use the same JSON
 * output module.
 */
const static char kJsonKey_ActualResults[] = "actual-results";
const static char kJsonKey_Header[] = "header";
const static char kJsonKey_Header_Type[] = "type";
const static char kJsonKey_Header_Revision[] = "revision";  // unique within Type
const static char kJsonKey_Image_ChecksumAlgorithm[] = "checksumAlgorithm";
const static char kJsonKey_Image_ChecksumValue[] = "checksumValue";
const static char kJsonKey_Image_ComparisonResult[] = "comparisonResult";
const static char kJsonKey_Image_Filepath[] = "filepath";
const static char kJsonKey_Source_TiledImages[] = "tiled-images";
const static char kJsonKey_Source_WholeImage[] = "whole-image";
// Values (not keys) that are written out by this JSON generator
const static char kJsonValue_Header_Type[] = "ChecksummedImages";
const static int kJsonValue_Header_Revision = 1;
const static char kJsonValue_Image_ChecksumAlgorithm_Bitmap64bitMD5[] = "bitmap-64bitMD5";
const static char kJsonValue_Image_ComparisonResult_NoComparison[] = "no-comparison";

void ImageResultsSummary::add(const char *sourceName, const char *fileName, uint64_t hash,
                              const int *tileNumber) {
    Json::Value image;
    image[kJsonKey_Image_ChecksumAlgorithm] = kJsonValue_Image_ChecksumAlgorithm_Bitmap64bitMD5;
    image[kJsonKey_Image_ChecksumValue] = Json::UInt64(hash);
    image[kJsonKey_Image_ComparisonResult] = kJsonValue_Image_ComparisonResult_NoComparison;
    image[kJsonKey_Image_Filepath] = fileName;
    if (NULL == tileNumber) {
        fActualResults[sourceName][kJsonKey_Source_WholeImage] = image;
    } else {
        fActualResults[sourceName][kJsonKey_Source_TiledImages][*tileNumber] = image;
    }
}

void ImageResultsSummary::add(const char *sourceName, const char *fileName, const SkBitmap& bitmap,
                              const int *tileNumber) {
    uint64_t hash;
    SkAssertResult(SkBitmapHasher::ComputeDigest(bitmap, &hash));
    this->add(sourceName, fileName, hash, tileNumber);
}

void ImageResultsSummary::writeToFile(const char *filename) {
    Json::Value header;
    header[kJsonKey_Header_Type] = kJsonValue_Header_Type;
    header[kJsonKey_Header_Revision] = kJsonValue_Header_Revision;
    Json::Value root;
    root[kJsonKey_Header] = header;
    root[kJsonKey_ActualResults] = fActualResults;
    std::string jsonStdString = root.toStyledString();
    SkFILEWStream stream(filename);
    stream.write(jsonStdString.c_str(), jsonStdString.length());
}

void PictureRenderer::init(SkPicture* pict, const SkString* outputDir,
                           const SkString* inputFilename, bool useChecksumBasedFilenames) {
    this->CopyString(&fOutputDir, outputDir);
    this->CopyString(&fInputFilename, inputFilename);
    fUseChecksumBasedFilenames = useChecksumBasedFilenames;

    SkASSERT(NULL == fPicture);
    SkASSERT(NULL == fCanvas.get());
    if (NULL != fPicture || NULL != fCanvas.get()) {
        return;
    }

    SkASSERT(pict != NULL);
    if (NULL == pict) {
        return;
    }

    fPicture.reset(pict)->ref();
    fCanvas.reset(this->setupCanvas());
}

void PictureRenderer::CopyString(SkString* dest, const SkString* src) {
    if (NULL != src) {
        dest->set(*src);
    } else {
        dest->reset();
    }
}

class FlagsDrawFilter : public SkDrawFilter {
public:
    FlagsDrawFilter(PictureRenderer::DrawFilterFlags* flags) :
        fFlags(flags) {}

    virtual bool filter(SkPaint* paint, Type t) {
        paint->setFlags(paint->getFlags() & ~fFlags[t] & SkPaint::kAllFlags);
        if (PictureRenderer::kMaskFilter_DrawFilterFlag & fFlags[t]) {
            SkMaskFilter* maskFilter = paint->getMaskFilter();
            if (NULL != maskFilter) {
                paint->setMaskFilter(NULL);
            }
        }
        if (PictureRenderer::kHinting_DrawFilterFlag & fFlags[t]) {
            paint->setHinting(SkPaint::kNo_Hinting);
        } else if (PictureRenderer::kSlightHinting_DrawFilterFlag & fFlags[t]) {
            paint->setHinting(SkPaint::kSlight_Hinting);
        }
        return true;
    }

private:
    PictureRenderer::DrawFilterFlags* fFlags;
};

static void setUpFilter(SkCanvas* canvas, PictureRenderer::DrawFilterFlags* drawFilters) {
    if (drawFilters && !canvas->getDrawFilter()) {
        canvas->setDrawFilter(SkNEW_ARGS(FlagsDrawFilter, (drawFilters)))->unref();
        if (drawFilters[0] & PictureRenderer::kAAClip_DrawFilterFlag) {
            canvas->setAllowSoftClip(false);
        }
    }
}

SkCanvas* PictureRenderer::setupCanvas() {
    const int width = this->getViewWidth();
    const int height = this->getViewHeight();
    return this->setupCanvas(width, height);
}

SkCanvas* PictureRenderer::setupCanvas(int width, int height) {
    SkCanvas* canvas;
    switch(fDeviceType) {
        case kBitmap_DeviceType: {
            SkBitmap bitmap;
            sk_tools::setup_bitmap(&bitmap, width, height);
            canvas = SkNEW_ARGS(SkCanvas, (bitmap));
        }
        break;
#if SK_SUPPORT_GPU
#if SK_ANGLE
        case kAngle_DeviceType:
            // fall through
#endif
#if SK_MESA
        case kMesa_DeviceType:
            // fall through
#endif
        case kGPU_DeviceType:
        case kNVPR_DeviceType: {
            SkAutoTUnref<GrSurface> target;
            if (fGrContext) {
                // create a render target to back the device
                GrTextureDesc desc;
                desc.fConfig = kSkia8888_GrPixelConfig;
                desc.fFlags = kRenderTarget_GrTextureFlagBit;
                desc.fWidth = width;
                desc.fHeight = height;
                desc.fSampleCnt = fSampleCount;
                target.reset(fGrContext->createUncachedTexture(desc, NULL, 0));
            }
            if (NULL == target.get()) {
                SkASSERT(0);
                return NULL;
            }

            SkAutoTUnref<SkGpuDevice> device(SkGpuDevice::Create(target));
            canvas = SkNEW_ARGS(SkCanvas, (device.get()));
            break;
        }
#endif
        default:
            SkASSERT(0);
            return NULL;
    }
    setUpFilter(canvas, fDrawFilters);
    this->scaleToScaleFactor(canvas);

    // Pictures often lie about their extent (i.e., claim to be 100x100 but
    // only ever draw to 90x100). Clear here so the undrawn portion will have
    // a consistent color
    canvas->clear(SK_ColorTRANSPARENT);
    return canvas;
}

void PictureRenderer::scaleToScaleFactor(SkCanvas* canvas) {
    SkASSERT(canvas != NULL);
    if (fScaleFactor != SK_Scalar1) {
        canvas->scale(fScaleFactor, fScaleFactor);
    }
}

void PictureRenderer::end() {
    this->resetState(true);
    fPicture.reset(NULL);
    fCanvas.reset(NULL);
}

int PictureRenderer::getViewWidth() {
    SkASSERT(fPicture != NULL);
    int width = SkScalarCeilToInt(fPicture->width() * fScaleFactor);
    if (fViewport.width() > 0) {
        width = SkMin32(width, fViewport.width());
    }
    return width;
}

int PictureRenderer::getViewHeight() {
    SkASSERT(fPicture != NULL);
    int height = SkScalarCeilToInt(fPicture->height() * fScaleFactor);
    if (fViewport.height() > 0) {
        height = SkMin32(height, fViewport.height());
    }
    return height;
}

/** Converts fPicture to a picture that uses a BBoxHierarchy.
 *  PictureRenderer subclasses that are used to test picture playback
 *  should call this method during init.
 */
void PictureRenderer::buildBBoxHierarchy() {
    SkASSERT(NULL != fPicture);
    if (kNone_BBoxHierarchyType != fBBoxHierarchyType && NULL != fPicture) {
        SkAutoTUnref<SkPictureFactory> factory(this->getFactory());
        SkPictureRecorder recorder(factory);
        SkCanvas* canvas = recorder.beginRecording(fPicture->width(), fPicture->height(),
                                                   this->recordFlags());
        fPicture->draw(canvas);
        fPicture.reset(recorder.endRecording());
    }
}

void PictureRenderer::resetState(bool callFinish) {
#if SK_SUPPORT_GPU
    SkGLContextHelper* glContext = this->getGLContext();
    if (NULL == glContext) {
        SkASSERT(kBitmap_DeviceType == fDeviceType);
        return;
    }

    fGrContext->flush();
    glContext->swapBuffers();
    if (callFinish) {
        SK_GL(*glContext, Finish());
    }
#endif
}

void PictureRenderer::purgeTextures() {
    SkDiscardableMemoryPool* pool = SkGetGlobalDiscardableMemoryPool();

    pool->dumpPool();

#if SK_SUPPORT_GPU
    SkGLContextHelper* glContext = this->getGLContext();
    if (NULL == glContext) {
        SkASSERT(kBitmap_DeviceType == fDeviceType);
        return;
    }

    // resetState should've already done this
    fGrContext->flush();

    fGrContext->purgeAllUnlockedResources();
#endif
}

uint32_t PictureRenderer::recordFlags() {
    return ((kNone_BBoxHierarchyType == fBBoxHierarchyType) ? 0 :
        SkPicture::kOptimizeForClippedPlayback_RecordingFlag) |
        SkPicture::kUsePathBoundsForClip_RecordingFlag;
}

/**
 * Write the canvas to the specified path.
 *
 * @param canvas Must be non-null. Canvas to be written to a file.
 * @param outputDir If nonempty, write the binary image to a file within this directory.
 * @param inputFilename If we are writing out a binary image, use this to build its filename.
 * @param jsonSummaryPtr If not null, add image results to this summary.
 * @param useChecksumBasedFilenames If true, use checksum-based filenames when writing to disk.
 * @param tileNumberPtr If not null, which tile number this image contains.
 * @return bool True if the Canvas is written to a file.
 *
 * TODO(epoger): Right now, all canvases must pass through this function in order to be appended
 * to the ImageResultsSummary.  We need some way to add bitmaps to the ImageResultsSummary
 * even if --writePath has not been specified (and thus this function is not called).
 *
 * One fix would be to pass in these path elements separately, and allow this function to be
 * called even if --writePath was not specified...
 *  const char *outputDir   // NULL if we don't want to write image files to disk
 *  const char *filename    // name we use within JSON summary, and as the filename within outputDir
 *
 * UPDATE: Now that outputDir and inputFilename are passed separately, we should be able to do that.
 */
static bool write(SkCanvas* canvas, const SkString& outputDir, const SkString& inputFilename,
                  ImageResultsSummary *jsonSummaryPtr, bool useChecksumBasedFilenames,
                  const int* tileNumberPtr=NULL) {
    SkASSERT(canvas != NULL);
    if (NULL == canvas) {
        return false;
    }

    SkBitmap bitmap;
    SkISize size = canvas->getDeviceSize();
    sk_tools::setup_bitmap(&bitmap, size.width(), size.height());

    // Make sure we only compute the bitmap hash once (at most).
    uint64_t hash;
    bool generatedHash = false;

    canvas->readPixels(&bitmap, 0, 0);
    sk_tools::force_all_opaque(bitmap);

    SkString escapedInputFilename(inputFilename);
    replace_char(&escapedInputFilename, '.', '_');

    // TODO(epoger): what about including the config type within outputFilename?  That way,
    // we could combine results of different config types without conflicting filenames.
    SkString outputFilename;
    const char *outputSubdirPtr = NULL;
    if (useChecksumBasedFilenames) {
        SkASSERT(!generatedHash);
        SkAssertResult(SkBitmapHasher::ComputeDigest(bitmap, &hash));
        generatedHash = true;

        outputSubdirPtr = escapedInputFilename.c_str();
        outputFilename.set(kJsonValue_Image_ChecksumAlgorithm_Bitmap64bitMD5);
        outputFilename.append("_");
        outputFilename.appendU64(hash);
    } else {
        outputFilename.set(escapedInputFilename);
        if (NULL != tileNumberPtr) {
            outputFilename.append("-tile");
            outputFilename.appendS32(*tileNumberPtr);
        }
    }
    outputFilename.append(".png");

    if (NULL != jsonSummaryPtr) {
        if (!generatedHash) {
            SkAssertResult(SkBitmapHasher::ComputeDigest(bitmap, &hash));
            generatedHash = true;
        }

        SkString outputRelativePath;
        if (outputSubdirPtr) {
            outputRelativePath.set(outputSubdirPtr);
            outputRelativePath.append("/");  // always use "/", even on Windows
            outputRelativePath.append(outputFilename);
        } else {
            outputRelativePath.set(outputFilename);
        }

        jsonSummaryPtr->add(inputFilename.c_str(), outputRelativePath.c_str(),
                            hash, tileNumberPtr);
    }

    SkASSERT(!outputDir.isEmpty()); // TODO(epoger): we want to remove this constraint,
                                    // as noted above
    SkString dirPath;
    if (outputSubdirPtr) {
        dirPath = SkOSPath::SkPathJoin(outputDir.c_str(), outputSubdirPtr);
        sk_mkdir(dirPath.c_str());
    } else {
        dirPath.set(outputDir);
    }
    SkString fullPath = SkOSPath::SkPathJoin(dirPath.c_str(), outputFilename.c_str());
    return SkImageEncoder::EncodeFile(fullPath.c_str(), bitmap, SkImageEncoder::kPNG_Type, 100);
}

///////////////////////////////////////////////////////////////////////////////////////////////

SkCanvas* RecordPictureRenderer::setupCanvas(int width, int height) {
    // defer the canvas setup until the render step
    return NULL;
}

// the size_t* parameter is deprecated, so we ignore it
static SkData* encode_bitmap_to_data(size_t*, const SkBitmap& bm) {
    return SkImageEncoder::EncodeData(bm, SkImageEncoder::kPNG_Type, 100);
}

bool RecordPictureRenderer::render(SkBitmap** out) {
    SkAutoTUnref<SkPictureFactory> factory(this->getFactory());
    SkPictureRecorder recorder(factory);
    SkCanvas* canvas = recorder.beginRecording(this->getViewWidth(), this->getViewHeight(),
                                               this->recordFlags());
    this->scaleToScaleFactor(canvas);
    fPicture->draw(canvas);
    SkAutoTUnref<SkPicture> picture(recorder.endRecording());
    if (!fOutputDir.isEmpty()) {
        // Record the new picture as a new SKP with PNG encoded bitmaps.
        SkString skpPath = SkOSPath::SkPathJoin(fOutputDir.c_str(), fInputFilename.c_str());
        SkFILEWStream stream(skpPath.c_str());
        picture->serialize(&stream, &encode_bitmap_to_data);
        return true;
    }
    return false;
}

SkString RecordPictureRenderer::getConfigNameInternal() {
    return SkString("record");
}

///////////////////////////////////////////////////////////////////////////////////////////////

bool PipePictureRenderer::render(SkBitmap** out) {
    SkASSERT(fCanvas.get() != NULL);
    SkASSERT(fPicture != NULL);
    if (NULL == fCanvas.get() || NULL == fPicture) {
        return false;
    }

    PipeController pipeController(fCanvas.get());
    SkGPipeWriter writer;
    SkCanvas* pipeCanvas = writer.startRecording(&pipeController);
    pipeCanvas->drawPicture(*fPicture);
    writer.endRecording();
    fCanvas->flush();
    if (!fOutputDir.isEmpty()) {
        return write(fCanvas, fOutputDir, fInputFilename, fJsonSummaryPtr,
                     fUseChecksumBasedFilenames);
    }
    if (NULL != out) {
        *out = SkNEW(SkBitmap);
        setup_bitmap(*out, fPicture->width(), fPicture->height());
        fCanvas->readPixels(*out, 0, 0);
    }
    return true;
}

SkString PipePictureRenderer::getConfigNameInternal() {
    return SkString("pipe");
}

///////////////////////////////////////////////////////////////////////////////////////////////

void SimplePictureRenderer::init(SkPicture* picture, const SkString* outputDir,
                                 const SkString* inputFilename, bool useChecksumBasedFilenames) {
    INHERITED::init(picture, outputDir, inputFilename, useChecksumBasedFilenames);
    this->buildBBoxHierarchy();
}

bool SimplePictureRenderer::render(SkBitmap** out) {
    SkASSERT(fCanvas.get() != NULL);
    SkASSERT(NULL != fPicture);
    if (NULL == fCanvas.get() || NULL == fPicture) {
        return false;
    }

    fCanvas->drawPicture(*fPicture);
    fCanvas->flush();
    if (!fOutputDir.isEmpty()) {
        return write(fCanvas, fOutputDir, fInputFilename, fJsonSummaryPtr,
                     fUseChecksumBasedFilenames);
    }

    if (NULL != out) {
        *out = SkNEW(SkBitmap);
        setup_bitmap(*out, fPicture->width(), fPicture->height());
        fCanvas->readPixels(*out, 0, 0);
    }

    return true;
}

SkString SimplePictureRenderer::getConfigNameInternal() {
    return SkString("simple");
}

///////////////////////////////////////////////////////////////////////////////////////////////

TiledPictureRenderer::TiledPictureRenderer()
    : fTileWidth(kDefaultTileWidth)
    , fTileHeight(kDefaultTileHeight)
    , fTileWidthPercentage(0.0)
    , fTileHeightPercentage(0.0)
    , fTileMinPowerOf2Width(0)
    , fCurrentTileOffset(-1)
    , fTilesX(0)
    , fTilesY(0) { }

void TiledPictureRenderer::init(SkPicture* pict, const SkString* outputDir,
                                const SkString* inputFilename, bool useChecksumBasedFilenames) {
    SkASSERT(NULL != pict);
    SkASSERT(0 == fTileRects.count());
    if (NULL == pict || fTileRects.count() != 0) {
        return;
    }

    // Do not call INHERITED::init(), which would create a (potentially large) canvas which is not
    // used by bench_pictures.
    fPicture.reset(pict)->ref();
    this->CopyString(&fOutputDir, outputDir);
    this->CopyString(&fInputFilename, inputFilename);
    fUseChecksumBasedFilenames = useChecksumBasedFilenames;
    this->buildBBoxHierarchy();

    if (fTileWidthPercentage > 0) {
        fTileWidth = sk_float_ceil2int(float(fTileWidthPercentage * fPicture->width() / 100));
    }
    if (fTileHeightPercentage > 0) {
        fTileHeight = sk_float_ceil2int(float(fTileHeightPercentage * fPicture->height() / 100));
    }

    if (fTileMinPowerOf2Width > 0) {
        this->setupPowerOf2Tiles();
    } else {
        this->setupTiles();
    }
    fCanvas.reset(this->setupCanvas(fTileWidth, fTileHeight));
    // Initialize to -1 so that the first call to nextTile will set this up to draw tile 0 on the
    // first call to drawCurrentTile.
    fCurrentTileOffset = -1;
}

void TiledPictureRenderer::end() {
    fTileRects.reset();
    this->INHERITED::end();
}

void TiledPictureRenderer::setupTiles() {
    // Only use enough tiles to cover the viewport
    const int width = this->getViewWidth();
    const int height = this->getViewHeight();

    fTilesX = fTilesY = 0;
    for (int tile_y_start = 0; tile_y_start < height; tile_y_start += fTileHeight) {
        fTilesY++;
        for (int tile_x_start = 0; tile_x_start < width; tile_x_start += fTileWidth) {
            if (0 == tile_y_start) {
                // Only count tiles in the X direction on the first pass.
                fTilesX++;
            }
            *fTileRects.append() = SkRect::MakeXYWH(SkIntToScalar(tile_x_start),
                                                    SkIntToScalar(tile_y_start),
                                                    SkIntToScalar(fTileWidth),
                                                    SkIntToScalar(fTileHeight));
        }
    }
}

bool TiledPictureRenderer::tileDimensions(int &x, int &y) {
    if (fTileRects.count() == 0 || NULL == fPicture) {
        return false;
    }
    x = fTilesX;
    y = fTilesY;
    return true;
}

// The goal of the powers of two tiles is to minimize the amount of wasted tile
// space in the width-wise direction and then minimize the number of tiles. The
// constraints are that every tile must have a pixel width that is a power of
// two and also be of some minimal width (that is also a power of two).
//
// This is solved by first taking our picture size and rounding it up to the
// multiple of the minimal width. The binary representation of this rounded
// value gives us the tiles we need: a bit of value one means we need a tile of
// that size.
void TiledPictureRenderer::setupPowerOf2Tiles() {
    // Only use enough tiles to cover the viewport
    const int width = this->getViewWidth();
    const int height = this->getViewHeight();

    int rounded_value = width;
    if (width % fTileMinPowerOf2Width != 0) {
        rounded_value = width - (width % fTileMinPowerOf2Width) + fTileMinPowerOf2Width;
    }

    int num_bits = SkScalarCeilToInt(scalar_log2(SkIntToScalar(width)));
    int largest_possible_tile_size = 1 << num_bits;

    fTilesX = fTilesY = 0;
    // The tile height is constant for a particular picture.
    for (int tile_y_start = 0; tile_y_start < height; tile_y_start += fTileHeight) {
        fTilesY++;
        int tile_x_start = 0;
        int current_width = largest_possible_tile_size;
        // Set fTileWidth to be the width of the widest tile, so that each canvas is large enough
        // to draw each tile.
        fTileWidth = current_width;

        while (current_width >= fTileMinPowerOf2Width) {
            // It is very important this is a bitwise AND.
            if (current_width & rounded_value) {
                if (0 == tile_y_start) {
                    // Only count tiles in the X direction on the first pass.
                    fTilesX++;
                }
                *fTileRects.append() = SkRect::MakeXYWH(SkIntToScalar(tile_x_start),
                                                        SkIntToScalar(tile_y_start),
                                                        SkIntToScalar(current_width),
                                                        SkIntToScalar(fTileHeight));
                tile_x_start += current_width;
            }

            current_width >>= 1;
        }
    }
}

/**
 * Draw the specified picture to the canvas translated to rectangle provided, so that this mini
 * canvas represents the rectangle's portion of the overall picture.
 * Saves and restores so that the initial clip and matrix return to their state before this function
 * is called.
 */
static void draw_tile_to_canvas(SkCanvas* canvas, const SkRect& tileRect, SkPicture* picture) {
    int saveCount = canvas->save();
    // Translate so that we draw the correct portion of the picture.
    // Perform a postTranslate so that the scaleFactor does not interfere with the positioning.
    SkMatrix mat(canvas->getTotalMatrix());
    mat.postTranslate(-tileRect.fLeft, -tileRect.fTop);
    canvas->setMatrix(mat);
    canvas->drawPicture(*picture);
    canvas->restoreToCount(saveCount);
    canvas->flush();
}

///////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Copies the entirety of the src bitmap (typically a tile) into a portion of the dst bitmap.
 * If the src bitmap is too large to fit within the dst bitmap after the x and y
 * offsets have been applied, any excess will be ignored (so only the top-left portion of the
 * src bitmap will be copied).
 *
 * @param src source bitmap
 * @param dst destination bitmap
 * @param xOffset x-offset within destination bitmap
 * @param yOffset y-offset within destination bitmap
 */
static void bitmapCopyAtOffset(const SkBitmap& src, SkBitmap* dst,
                               int xOffset, int yOffset) {
    for (int y = 0; y <src.height() && y + yOffset < dst->height() ; y++) {
        for (int x = 0; x < src.width() && x + xOffset < dst->width() ; x++) {
            *dst->getAddr32(xOffset + x, yOffset + y) = *src.getAddr32(x, y);
        }
    }
}

bool TiledPictureRenderer::nextTile(int &i, int &j) {
    if (++fCurrentTileOffset < fTileRects.count()) {
        i = fCurrentTileOffset % fTilesX;
        j = fCurrentTileOffset / fTilesX;
        return true;
    }
    return false;
}

void TiledPictureRenderer::drawCurrentTile() {
    SkASSERT(fCurrentTileOffset >= 0 && fCurrentTileOffset < fTileRects.count());
    draw_tile_to_canvas(fCanvas, fTileRects[fCurrentTileOffset], fPicture);
}

bool TiledPictureRenderer::render(SkBitmap** out) {
    SkASSERT(fPicture != NULL);
    if (NULL == fPicture) {
        return false;
    }

    SkBitmap bitmap;
    if (out){
        *out = SkNEW(SkBitmap);
        setup_bitmap(*out, fPicture->width(), fPicture->height());
        setup_bitmap(&bitmap, fTileWidth, fTileHeight);
    }
    bool success = true;
    for (int i = 0; i < fTileRects.count(); ++i) {
        draw_tile_to_canvas(fCanvas, fTileRects[i], fPicture);
        if (!fOutputDir.isEmpty()) {
            success &= write(fCanvas, fOutputDir, fInputFilename, fJsonSummaryPtr,
                             fUseChecksumBasedFilenames, &i);
        }
        if (NULL != out) {
            if (fCanvas->readPixels(&bitmap, 0, 0)) {
                // Add this tile to the entire bitmap.
                bitmapCopyAtOffset(bitmap, *out, SkScalarFloorToInt(fTileRects[i].left()),
                                   SkScalarFloorToInt(fTileRects[i].top()));
            } else {
                success = false;
            }
        }
    }
    return success;
}

SkCanvas* TiledPictureRenderer::setupCanvas(int width, int height) {
    SkCanvas* canvas = this->INHERITED::setupCanvas(width, height);
    SkASSERT(NULL != fPicture);
    // Clip the tile to an area that is completely inside both the SkPicture and the viewport. This
    // is mostly important for tiles on the right and bottom edges as they may go over this area and
    // the picture may have some commands that draw outside of this area and so should not actually
    // be written.
    // Uses a clipRegion so that it will be unaffected by the scale factor, which may have been set
    // by INHERITED::setupCanvas.
    SkRegion clipRegion;
    clipRegion.setRect(0, 0, this->getViewWidth(), this->getViewHeight());
    canvas->clipRegion(clipRegion);
    return canvas;
}

SkString TiledPictureRenderer::getConfigNameInternal() {
    SkString name;
    if (fTileMinPowerOf2Width > 0) {
        name.append("pow2tile_");
        name.appendf("%i", fTileMinPowerOf2Width);
    } else {
        name.append("tile_");
        if (fTileWidthPercentage > 0) {
            name.appendf("%.f%%", fTileWidthPercentage);
        } else {
            name.appendf("%i", fTileWidth);
        }
    }
    name.append("x");
    if (fTileHeightPercentage > 0) {
        name.appendf("%.f%%", fTileHeightPercentage);
    } else {
        name.appendf("%i", fTileHeight);
    }
    return name;
}

///////////////////////////////////////////////////////////////////////////////////////////////

// Holds all of the information needed to draw a set of tiles.
class CloneData : public SkRunnable {

public:
    CloneData(SkPicture* clone, SkCanvas* canvas, SkTDArray<SkRect>& rects, int start, int end,
              SkRunnable* done, ImageResultsSummary* jsonSummaryPtr, bool useChecksumBasedFilenames)
        : fClone(clone)
        , fCanvas(canvas)
        , fRects(rects)
        , fStart(start)
        , fEnd(end)
        , fSuccess(NULL)
        , fDone(done)
        , fJsonSummaryPtr(jsonSummaryPtr)
        , fUseChecksumBasedFilenames(useChecksumBasedFilenames) {
        SkASSERT(fDone != NULL);
    }

    virtual void run() SK_OVERRIDE {
        SkGraphics::SetTLSFontCacheLimit(1024 * 1024);

        SkBitmap bitmap;
        if (fBitmap != NULL) {
            // All tiles are the same size.
            setup_bitmap(&bitmap, SkScalarFloorToInt(fRects[0].width()), SkScalarFloorToInt(fRects[0].height()));
        }

        for (int i = fStart; i < fEnd; i++) {
            draw_tile_to_canvas(fCanvas, fRects[i], fClone);
            if ((!fOutputDir.isEmpty())
                && !write(fCanvas, fOutputDir, fInputFilename, fJsonSummaryPtr,
                          fUseChecksumBasedFilenames, &i)
                && fSuccess != NULL) {
                *fSuccess = false;
                // If one tile fails to write to a file, do not continue drawing the rest.
                break;
            }
            if (fBitmap != NULL) {
                if (fCanvas->readPixels(&bitmap, 0, 0)) {
                    SkAutoLockPixels alp(*fBitmap);
                    bitmapCopyAtOffset(bitmap, fBitmap, SkScalarFloorToInt(fRects[i].left()),
                                       SkScalarFloorToInt(fRects[i].top()));
                } else {
                    *fSuccess = false;
                    // If one tile fails to read pixels, do not continue drawing the rest.
                    break;
                }
            }
        }
        fDone->run();
    }

    void setPathsAndSuccess(const SkString& outputDir, const SkString& inputFilename,
                            bool* success) {
        fOutputDir.set(outputDir);
        fInputFilename.set(inputFilename);
        fSuccess = success;
    }

    void setBitmap(SkBitmap* bitmap) {
        fBitmap = bitmap;
    }

private:
    // All pointers unowned.
    SkPicture*         fClone;      // Picture to draw from. Each CloneData has a unique one which
                                    // is threadsafe.
    SkCanvas*          fCanvas;     // Canvas to draw to. Reused for each tile.
    SkString           fOutputDir;  // If not empty, write results into this directory.
    SkString           fInputFilename; // Filename of input SkPicture file.
    SkTDArray<SkRect>& fRects;      // All tiles of the picture.
    const int          fStart;      // Range of tiles drawn by this thread.
    const int          fEnd;
    bool*              fSuccess;    // Only meaningful if path is non-null. Shared by all threads,
                                    // and only set to false upon failure to write to a PNG.
    SkRunnable*        fDone;
    SkBitmap*          fBitmap;
    ImageResultsSummary* fJsonSummaryPtr;
    bool               fUseChecksumBasedFilenames;
};

MultiCorePictureRenderer::MultiCorePictureRenderer(int threadCount)
: fNumThreads(threadCount)
, fThreadPool(threadCount)
, fCountdown(threadCount) {
    // Only need to create fNumThreads - 1 clones, since one thread will use the base
    // picture.
    fPictureClones = SkNEW_ARRAY(SkPicture, fNumThreads - 1);
    fCloneData = SkNEW_ARRAY(CloneData*, fNumThreads);
}

void MultiCorePictureRenderer::init(SkPicture *pict, const SkString* outputDir,
                                    const SkString* inputFilename, bool useChecksumBasedFilenames) {
    // Set fPicture and the tiles.
    this->INHERITED::init(pict, outputDir, inputFilename, useChecksumBasedFilenames);
    for (int i = 0; i < fNumThreads; ++i) {
        *fCanvasPool.append() = this->setupCanvas(this->getTileWidth(), this->getTileHeight());
    }
    // Only need to create fNumThreads - 1 clones, since one thread will use the base picture.
    fPicture->clone(fPictureClones, fNumThreads - 1);
    // Populate each thread with the appropriate data.
    // Group the tiles into nearly equal size chunks, rounding up so we're sure to cover them all.
    const int chunkSize = (fTileRects.count() + fNumThreads - 1) / fNumThreads;

    for (int i = 0; i < fNumThreads; i++) {
        SkPicture* pic;
        if (i == fNumThreads-1) {
            // The last set will use the original SkPicture.
            pic = fPicture;
        } else {
            pic = &fPictureClones[i];
        }
        const int start = i * chunkSize;
        const int end = SkMin32(start + chunkSize, fTileRects.count());
        fCloneData[i] = SkNEW_ARGS(CloneData,
                                   (pic, fCanvasPool[i], fTileRects, start, end, &fCountdown,
                                    fJsonSummaryPtr, useChecksumBasedFilenames));
    }
}

bool MultiCorePictureRenderer::render(SkBitmap** out) {
    bool success = true;
    if (!fOutputDir.isEmpty()) {
        for (int i = 0; i < fNumThreads-1; i++) {
            fCloneData[i]->setPathsAndSuccess(fOutputDir, fInputFilename, &success);
        }
    }

    if (NULL != out) {
        *out = SkNEW(SkBitmap);
        setup_bitmap(*out, fPicture->width(), fPicture->height());
        for (int i = 0; i < fNumThreads; i++) {
            fCloneData[i]->setBitmap(*out);
        }
    } else {
        for (int i = 0; i < fNumThreads; i++) {
            fCloneData[i]->setBitmap(NULL);
        }
    }

    fCountdown.reset(fNumThreads);
    for (int i = 0; i < fNumThreads; i++) {
        fThreadPool.add(fCloneData[i]);
    }
    fCountdown.wait();

    return success;
}

void MultiCorePictureRenderer::end() {
    for (int i = 0; i < fNumThreads - 1; i++) {
        SkDELETE(fCloneData[i]);
        fCloneData[i] = NULL;
    }

    fCanvasPool.unrefAll();

    this->INHERITED::end();
}

MultiCorePictureRenderer::~MultiCorePictureRenderer() {
    // Each individual CloneData was deleted in end.
    SkDELETE_ARRAY(fCloneData);
    SkDELETE_ARRAY(fPictureClones);
}

SkString MultiCorePictureRenderer::getConfigNameInternal() {
    SkString name = this->INHERITED::getConfigNameInternal();
    name.appendf("_multi_%i_threads", fNumThreads);
    return name;
}

///////////////////////////////////////////////////////////////////////////////////////////////

void PlaybackCreationRenderer::setup() {
    SkAutoTUnref<SkPictureFactory> factory(this->getFactory());
    fRecorder.reset(SkNEW_ARGS(SkPictureRecorder, (factory)));
    SkCanvas* canvas = fRecorder->beginRecording(this->getViewWidth(), this->getViewHeight(),
                                                 this->recordFlags());
    this->scaleToScaleFactor(canvas);
    canvas->drawPicture(*fPicture);
}

bool PlaybackCreationRenderer::render(SkBitmap** out) {
    fPicture.reset(fRecorder->endRecording());
    // Since this class does not actually render, return false.
    return false;
}

SkString PlaybackCreationRenderer::getConfigNameInternal() {
    return SkString("playback_creation");
}

///////////////////////////////////////////////////////////////////////////////////////////////
// SkPicture variants for each BBoxHierarchy type

class RTreePicture : public SkPicture {
public:
    virtual SkBBoxHierarchy* createBBoxHierarchy() const SK_OVERRIDE {
        static const int kRTreeMinChildren = 6;
        static const int kRTreeMaxChildren = 11;
        SkScalar aspectRatio = SkScalarDiv(SkIntToScalar(fWidth),
                                           SkIntToScalar(fHeight));
        bool sortDraws = false;
        return SkRTree::Create(kRTreeMinChildren, kRTreeMaxChildren,
                               aspectRatio, sortDraws);
    }
};

class SkRTreePictureFactory : public SkPictureFactory {
private:
    virtual SkPicture* create(int width, int height) SK_OVERRIDE {
        return SkNEW(RTreePicture);
    }

private:
    typedef SkPictureFactory INHERITED;
};

SkPictureFactory* PictureRenderer::getFactory() {
    switch (fBBoxHierarchyType) {
        case kNone_BBoxHierarchyType:
            return NULL;
        case kQuadTree_BBoxHierarchyType:
            return SkNEW(SkQuadTreePictureFactory);
        case kRTree_BBoxHierarchyType:
            return SkNEW(SkRTreePictureFactory);
        case kTileGrid_BBoxHierarchyType:
            return new SkTileGridPictureFactory(fGridInfo);
    }
    SkASSERT(0); // invalid bbhType
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////

class GatherRenderer : public PictureRenderer {
public:
    virtual bool render(SkBitmap** out = NULL) SK_OVERRIDE {
        SkRect bounds = SkRect::MakeWH(SkIntToScalar(fPicture->width()),
                                       SkIntToScalar(fPicture->height()));
        SkData* data = SkPictureUtils::GatherPixelRefs(fPicture, bounds);
        SkSafeUnref(data);

        return (fOutputDir.isEmpty());    // we don't have anything to write
    }

private:
    virtual SkString getConfigNameInternal() SK_OVERRIDE {
        return SkString("gather_pixelrefs");
    }
};

PictureRenderer* CreateGatherPixelRefsRenderer() {
    return SkNEW(GatherRenderer);
}

///////////////////////////////////////////////////////////////////////////////

class PictureCloneRenderer : public PictureRenderer {
public:
    virtual bool render(SkBitmap** out = NULL) SK_OVERRIDE {
        for (int i = 0; i < 100; ++i) {
            SkPicture* clone = fPicture->clone();
            SkSafeUnref(clone);
        }

        return (fOutputDir.isEmpty());    // we don't have anything to write
    }

private:
    virtual SkString getConfigNameInternal() SK_OVERRIDE {
        return SkString("picture_clone");
    }
};

PictureRenderer* CreatePictureCloneRenderer() {
    return SkNEW(PictureCloneRenderer);
}

} // namespace sk_tools
