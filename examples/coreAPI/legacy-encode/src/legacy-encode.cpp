//==============================================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
//==============================================================================

///
/// A minimal oneAPI Video Processing Library (oneVPL) encode application
/// using the core API subset.  For more information see:
/// https://software.intel.com/content/www/us/en/develop/articles/upgrading-from-msdk-to-onevpl.html
/// https://oneapi-src.github.io/oneAPI-spec/elements/oneVPL/source/index.html
/// @file

#include "util.h"

#define TARGETKBPS            4000
#define FRAMERATE             30
#define OUTPUT_FILE           "out.h265"
#define BITSTREAM_BUFFER_SIZE 2000000

void Usage(void) {
    printf("\n");
    printf("   Usage  :  legacy-encode\n");
    printf("     -hw        use hardware implementation\n");
    printf("     -sw        use software implementation\n");
    printf("     -i input file name ( -sw=I420 raw frames,-hw=NV12)\n");
    printf("     -w input width\n");
    printf("     -h input height\n\n");
    printf("   Example:  legacy-encode -sw -i in.i420 -w 320 -h 240\n");
    printf("   To view:  ffplay %s\n\n", OUTPUT_FILE);
    printf(" * Encode raw frames to HEVC/H265 elementary stream in %s\n\n", OUTPUT_FILE);
    return;
}

int main(int argc, char *argv[]) {
    FILE *source = NULL;
    FILE *sink   = NULL;
    int accel_fd = 0;
    mfxConfig cfg[1];
    mfxSession session              = NULL;
    mfxVideoParam encodeParams      = {};
    mfxFrameSurface1 *encSurfaceIn  = NULL;
    mfxFrameSurface1 *encSurfPool   = NULL;
    mfxLoader loader                = NULL;
    mfxU8 *encOutBuf                = NULL;
    void *accelHandle               = NULL;
    mfxBitstream bitstream          = {};
    mfxSyncPoint syncp              = {};
    mfxFrameAllocRequest encRequest = {};
    mfxU32 framenum                 = 0;
    bool isDraining                 = false;
    bool isStillGoing               = true;
    int nIndex                      = -1;
    mfxStatus sts                   = MFX_ERR_NONE;
    Params cliParams                = {};

    //Parse command line args to cliParams
    if (ParseArgsAndValidate(argc, argv, &cliParams, PARAMS_ENCODE) == false) {
        Usage();
        return 1; // return 1 as error code
    }

    source = fopen(cliParams.infileName, "rb");
    VERIFY(source, "Could not open input file");

    sink = fopen(OUTPUT_FILE, "wb");
    VERIFY(sink, "Could not create output file");

    // Initialize oneVPL session
    loader = MFXLoad();
    VERIFY(NULL != loader, "MFXLoad failed -- is implementation in path?");

    // Implementation used must be the type requested from command line
    cfg[0] = MFXCreateConfig(loader);
    VERIFY(NULL != cfg[0], "MFXCreateConfig failed")

    sts =
        MFXSetConfigFilterProperty(cfg[0], (mfxU8 *)"mfxImplDescription.Impl", cliParams.implValue);
    VERIFY(MFX_ERR_NONE == sts, "MFXSetConfigFilterProperty failed for Impl");

    sts = MFXCreateSession(loader, 0, &session);
    VERIFY(MFX_ERR_NONE == sts,
           "Cannot create session -- no implementations meet selection criteria");

    // Print info about implementation loaded
    ShowImplementationInfo(loader, 0);

    // Convenience function to initialize available accelerator(s)
    accelHandle = InitAcceleratorHandle(session, &accel_fd);

    // Initialize encode parameters
    encodeParams.mfx.CodecId                 = MFX_CODEC_HEVC;
    encodeParams.mfx.TargetUsage             = MFX_TARGETUSAGE_BALANCED;
    encodeParams.mfx.TargetKbps              = TARGETKBPS;
    encodeParams.mfx.RateControlMethod       = MFX_RATECONTROL_VBR;
    encodeParams.mfx.FrameInfo.FrameRateExtN = FRAMERATE;
    encodeParams.mfx.FrameInfo.FrameRateExtD = 1;
    if (MFX_IMPL_SOFTWARE == cliParams.impl) {
        encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_I420;
    }
    else {
        encodeParams.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    }
    encodeParams.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    encodeParams.mfx.FrameInfo.CropX        = 0;
    encodeParams.mfx.FrameInfo.CropY        = 0;
    encodeParams.mfx.FrameInfo.CropW        = cliParams.srcWidth;
    encodeParams.mfx.FrameInfo.CropH        = cliParams.srcHeight;
    encodeParams.mfx.FrameInfo.Width        = ALIGN16(cliParams.srcWidth);
    encodeParams.mfx.FrameInfo.Height       = ALIGN16(cliParams.srcHeight);

    encodeParams.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    //fill in missing params
    sts = MFXVideoENCODE_Query(session, &encodeParams, &encodeParams);
    VERIFY(MFX_ERR_NONE == sts, "Encode query failed");

    // Initialize encoder
    sts = MFXVideoENCODE_Init(session, &encodeParams);
    VERIFY(MFX_ERR_NONE == sts, "Encode init failed");

    // Query number required surfaces for decoder
    sts = MFXVideoENCODE_QueryIOSurf(session, &encodeParams, &encRequest);
    VERIFY(MFX_ERR_NONE == sts, "QueryIOSurf failed");

    // Prepare output bitstream
    bitstream.MaxLength = BITSTREAM_BUFFER_SIZE;
    bitstream.Data      = (mfxU8 *)malloc(bitstream.MaxLength * sizeof(mfxU8));

    // External (application) allocation of decode surfaces
    encSurfPool =
        (mfxFrameSurface1 *)calloc(sizeof(mfxFrameSurface1), encRequest.NumFrameSuggested);
    sts = AllocateExternalSystemMemorySurfacePool(&encOutBuf,
                                                  encSurfPool,
                                                  encodeParams.mfx.FrameInfo,
                                                  encRequest.NumFrameSuggested);
    VERIFY(MFX_ERR_NONE == sts, "Error in external surface allocation\n");

    // ===================================
    // Start encoding the frames
    //
    printf("Encoding %s -> %s\n", cliParams.infileName, OUTPUT_FILE);

    while (isStillGoing == true) {
        // Load a new frame if not draining
        if (isDraining == false) {
            nIndex       = GetFreeSurfaceIndex(encSurfPool, encRequest.NumFrameSuggested);
            encSurfaceIn = &encSurfPool[nIndex];

            sts = ReadRawFrame(encSurfaceIn, source);
            if (sts != MFX_ERR_NONE)
                isDraining = true;
        }

        sts = MFXVideoENCODE_EncodeFrameAsync(session,
                                              NULL,
                                              (isDraining == true) ? NULL : encSurfaceIn,
                                              &bitstream,
                                              &syncp);

        switch (sts) {
            case MFX_ERR_NONE:
                // MFX_ERR_NONE and syncp indicate output is available
                if (syncp) {
                    // Encode output is not available on CPU until sync operation completes
                    sts = MFXVideoCORE_SyncOperation(session, syncp, WAIT_100_MILLISECONDS);
                    VERIFY(MFX_ERR_NONE == sts, "MFXVideoCORE_SyncOperation error");

                    WriteEncodedStream(bitstream, sink);
                    framenum++;
                }
                break;
            case MFX_ERR_NOT_ENOUGH_BUFFER:
                // This example deliberatly uses a large output buffer with immediate write to disk
                // for simplicity.
                // Handle when frame size exceeds available buffer here
                break;
            case MFX_ERR_MORE_DATA:
                // The function requires more data to generate any output
                if (isDraining)
                    isStillGoing = false;
                break;
            case MFX_ERR_DEVICE_LOST:
                // For non-CPU implementations,
                // Cleanup if device is lost
                break;
            case MFX_WRN_DEVICE_BUSY:
                // For non-CPU implementations,
                // Wait a few milliseconds then try again
                break;
            default:
                printf("unknown status %d\n", sts);
                isStillGoing = false;
                break;
        }
    }

end:
    printf("Encoded %d frames\n", framenum);

    // Clean up resources - It is recommended to close components first, before
    // releasing allocated surfaces, since some surfaces may still be locked by
    // internal resources.
    if (session) {
        MFXVideoENCODE_Close(session);
        MFXClose(session);
    }

    if (bitstream.Data)
        free(bitstream.Data);

    if (encSurfPool || encOutBuf) {
        FreeExternalSystemMemorySurfacePool(encOutBuf, encSurfPool);
    }

    if (source)
        fclose(source);

    if (sink)
        fclose(sink);

    FreeAcceleratorHandle(accelHandle, accel_fd);
    accelHandle = NULL;
    accel_fd    = 0;

    if (loader)
        MFXUnload(loader);

    return 0;
}
