// Copyright 2022 Peter Leontev. All Rights Reserved.

#include "RuntimeImageUtils.h"

#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "PixelFormat.h"
#include "HAL/FileManager.h"
#include "HAL/UnrealMemory.h"
#include "Serialization/BulkData.h"
#include "Serialization/Archive.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "Runtime/Launch/Resources/Version.h"

#include "Helpers/TGAHelpers.h"
#include "Helpers/PNGHelpers.h"
#include "Helpers/TIFFLoader.h"


namespace FRuntimeImageUtils
{
    bool IsImportResolutionValid(int32 Width, int32 Height, bool bAllowNonPowerOfTwo)
    {
        // TODO: 
        const int32 MAX_TEXTURE_SIZE = 8192;

        // Calculate the maximum supported resolution utilizing the global max texture mip count
        // (Note, have to subtract 1 because 1x1 is a valid mip-size; this means a GMaxTextureMipCount of 4 means a max resolution of 8x8, not 2^4 = 16x16)
        const int32 MaximumSupportedResolution = 1 << (GMaxTextureMipCount - 1);

        bool bValid = true;

        // Check if the texture is above the supported resolution and prompt the user if they wish to continue if it is
        if (Width > MaximumSupportedResolution || Height > MaximumSupportedResolution)
        {
            bValid = false;
        }

        const bool bIsPowerOfTwo = FMath::IsPowerOfTwo(Width) && FMath::IsPowerOfTwo(Height);
        // Check if the texture dimensions are powers of two
        if (!bAllowNonPowerOfTwo && !bIsPowerOfTwo)
        {
            bValid = false;
        }

        if (Width > MAX_TEXTURE_SIZE || Height > MAX_TEXTURE_SIZE)
        {
            bValid = false;
        }

        return bValid;
    }

    bool ImportBufferAsImage(const uint8* Buffer, int32 Length, FRuntimeImageData& OutImage, FString& OutError)
    {
        QUICK_SCOPE_CYCLE_COUNTER(STAT_EvoImageUtils_ImportFileAsTexture_ImportBufferAsImage);
        
        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

        //
        // PNG
        //
        // PNG support both 8 and 16 bit depth images (24 and 48 bits per pixel respectively or 32 and 64 bits when alpha channel is used) 
        TSharedPtr<IImageWrapper> PngImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (PngImageWrapper.IsValid() && PngImageWrapper->SetCompressed(Buffer, Length))
        {
            if (!IsImportResolutionValid(PngImageWrapper->GetWidth(), PngImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), PngImageWrapper->GetWidth(), PngImageWrapper->GetHeight());
                return false;
            }

            // Select the texture's source format
            ETextureSourceFormat TextureFormat = TSF_Invalid;
            int32 BitDepth = PngImageWrapper->GetBitDepth();
            ERGBFormat Format = PngImageWrapper->GetFormat();

            if (Format == ERGBFormat::Gray)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_G8;
                    Format = ERGBFormat::Gray;
                    BitDepth = 8;
                }
                else if (BitDepth == 16)
                {
                    // TODO: TSF_G16?
                    TextureFormat = TSF_RGBA16;
                    Format = ERGBFormat::RGBA;
                    BitDepth = 16;
                }
            }
            else if (Format == ERGBFormat::RGBA || Format == ERGBFormat::BGRA)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_BGRA8;
                    Format = ERGBFormat::BGRA;
                    BitDepth = 8;
                }
                else if (BitDepth == 16)
                {
                    TextureFormat = TSF_RGBA16;
                    Format = ERGBFormat::RGBA;
                    BitDepth = 16;
                }
            }

            if (BitDepth > 16)
            {
                OutError = TEXT("Only 8 and 16 bit depth PNG images are currently supported.");
                return false;
            }

            TArray<uint8> RawPNG;
            if (PngImageWrapper->GetRaw(Format, BitDepth, RawPNG))
            {
                OutImage.Init2D(
                    PngImageWrapper->GetWidth(),
                    PngImageWrapper->GetHeight(),
                    TextureFormat,
                    RawPNG.GetData()
                );
                OutImage.SRGB = BitDepth < 16;
                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear; 

                FPNGHelpers::FillZeroAlphaPNGData(OutImage.SizeX, OutImage.SizeY, OutImage.TextureSourceFormat, OutImage.RawData.GetData());
            }
            else
            {
                OutError = FString::Printf(TEXT("Failed to decode PNG. Bit depth: %d"), BitDepth);
                return false;
            }

            return true;
        }

        //
        // JPEG
        //
        // JPEG can only be 8-bit depth

        TSharedPtr<IImageWrapper> JpegImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        if (JpegImageWrapper.IsValid() && JpegImageWrapper->SetCompressed(Buffer, Length))
        {
            if (!IsImportResolutionValid(JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), JpegImageWrapper->GetWidth(), JpegImageWrapper->GetHeight());
                return false;
            }

            // Select the texture's source format
            ETextureSourceFormat TextureFormat = TSF_Invalid;
            int32 BitDepth = JpegImageWrapper->GetBitDepth();
            ERGBFormat Format = JpegImageWrapper->GetFormat();

            if (Format == ERGBFormat::Gray)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_G8;
                    Format = ERGBFormat::Gray;
                    BitDepth = 8;
                }
            }
            else if (Format == ERGBFormat::RGBA)
            {
                if (BitDepth <= 8)
                {
                    TextureFormat = TSF_BGRA8;
                    Format = ERGBFormat::BGRA;
                    BitDepth = 8;
                }
            }

            if (TextureFormat == TSF_Invalid)
            {
                OutError = FString::Printf(TEXT("JPEG file contains data in an unsupported format. Bit depth: %d"), BitDepth);
                return false;
            }

            TArray<uint8> RawJPEG;
            if (JpegImageWrapper->GetRaw(Format, BitDepth, RawJPEG))
            {
                OutImage.Init2D(
                    JpegImageWrapper->GetWidth(),
                    JpegImageWrapper->GetHeight(),
                    TextureFormat,
                    RawJPEG.GetData()
                );
                OutImage.SRGB = true;
                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
            }
            else
            {
                OutError = TEXT("Failed to decode JPEG. Please contact devs");
                return false;
            }

            return true;
        }

        //
        // BMP
        //
        TSharedPtr<IImageWrapper> BmpImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::BMP);
        if (BmpImageWrapper.IsValid() && BmpImageWrapper->SetCompressed(Buffer, Length))
        {
            // Check the resolution of the imported texture to ensure validity
            if (!IsImportResolutionValid(BmpImageWrapper->GetWidth(), BmpImageWrapper->GetHeight(), true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), BmpImageWrapper->GetWidth(), BmpImageWrapper->GetHeight());
                return false;
            }

            TArray<uint8> RawBMP;
            if (BmpImageWrapper->GetRaw(BmpImageWrapper->GetFormat(), BmpImageWrapper->GetBitDepth(), RawBMP))
            {
                // Set texture properties.
                OutImage.Init2D(
                    BmpImageWrapper->GetWidth(),
                    BmpImageWrapper->GetHeight(),
                    TSF_BGRA8,
                    RawBMP.GetData()
                );
                
                OutImage.SRGB = true;
                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
            }
            else
            {
                OutError = FString::Printf(TEXT("Failed to decode BMP. Bit depth: %d"), BmpImageWrapper->GetBitDepth());
                return false;
            }

            return true;
        }
       
        //
        // TGA
        //
        // Support for alpha stored as pseudo-color 8-bit TGA
        const FTGAHelpers::FTGAFileHeader* TGA = (FTGAHelpers::FTGAFileHeader*)Buffer;
        if (Length >= sizeof(FTGAHelpers::FTGAFileHeader) &&
            ((TGA->ColorMapType == 0 && TGA->ImageTypeCode == 2) ||
            // ImageTypeCode 3 is greyscale
            (TGA->ColorMapType == 0 && TGA->ImageTypeCode == 3) ||
            (TGA->ColorMapType == 0 && TGA->ImageTypeCode == 10) ||
            (TGA->ColorMapType == 1 && TGA->ImageTypeCode == 1 && TGA->BitsPerPixel == 8))
            )
        {
            // Check the resolution of the imported texture to ensure validity
            if (!IsImportResolutionValid(TGA->Width, TGA->Height, true))
            {
                OutError = FString::Printf(TEXT("Texture resolution is not supported: %d x %d"), TGA->Width, TGA->Height);
                return false;
            }

            const bool bResult = FTGAHelpers::DecompressTGA(TGA, OutImage, OutError);
            if (bResult)
            {
                if (OutImage.CompressionSettings == TC_Grayscale && TGA->ImageTypeCode == 3)
                {
                    // default grayscales to linear as they wont get compression otherwise and are commonly used as masks
                    OutImage.SRGB = false;
                    
                }

                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
            }
            else
            {
                OutError = TEXT("Failed to decompress TGA. Please contact devs");
                return false;
            }
        }
        /*else
        {
            OutError = TEXT("TGA file contains data in an unsupported format. Please contact devs");
            return false;
        }*/


        //
        // OpenEXR
        //
        TSharedPtr<IImageWrapper> ExrImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::EXR);
        if (ExrImageWrapper.IsValid() && ExrImageWrapper->SetCompressed(Buffer, Length))
        {
            int32 Width = ExrImageWrapper->GetWidth();
            int32 Height = ExrImageWrapper->GetHeight();

            if (!IsImportResolutionValid(Width, Height, true))
            {
                return false;
            }

            // Select the texture's source format
            ETextureSourceFormat TextureFormat = TSF_Invalid;
            int32 BitDepth = ExrImageWrapper->GetBitDepth();
            ERGBFormat Format = ExrImageWrapper->GetFormat();

            if (Format == ERGBFormat::RGBA && BitDepth == 16)
            {
                TextureFormat = TSF_RGBA16F;
                Format = ERGBFormat::BGRA;
            }

            if (TextureFormat == TSF_Invalid)
            {
                OutError = TEXT("EXR file contains data in an unsupported format.");
                return false;
            }

            TArray<uint8> RawExr;
            if (ExrImageWrapper->GetRaw(Format, BitDepth, RawExr))
            {
                OutImage.Init2D(
                    Width,
                    Height,
                    TextureFormat,
                    RawExr.GetData()
                );

                OutImage.SRGB = false;
                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
                OutImage.CompressionSettings = TC_HDR;
            }
            else
            {
                OutError = FString::Printf(TEXT("Failed to decode EXR. Bit depth: %d"), BitDepth);
                return false;
            }

            return true;
        }

#if WITH_FREEIMAGE_LIB
        FRuntimeTiffLoadHelper TiffLoaderHelper;
        if (TiffLoaderHelper.IsValid())
        {
            if (TiffLoaderHelper.Load(Buffer, Length))
            {
                OutImage.Init2D(
                    TiffLoaderHelper.Width,
                    TiffLoaderHelper.Height,
                    TiffLoaderHelper.TextureSourceFormat,
                    TiffLoaderHelper.RawData.GetData()
                );

                OutImage.SRGB = TiffLoaderHelper.bSRGB;
                OutImage.GammaSpace = OutImage.SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
                OutImage.CompressionSettings = TiffLoaderHelper.CompressionSettings;

                return true;
            }
            else
            {
                OutError = FString::Printf(TEXT("Failed to decode TIFF. Bits per pixel: %d"), TiffLoaderHelper.GetBitDepth());
                return false;
            }
#endif // WITH_FREEIMAGE_LIB
        }

        return true;
    }

    void ImportFileAsImage(const FString& ImageFilename, FRuntimeImageData& OutImage, FString& OutError)
    {
        QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageUtils_ImportFileAsTexture);

        // TODO: 
        const int64 MAX_FILESIZE_BYTES = 999999999;

        IFileManager& FileManager = IFileManager::Get();

        if (!FileManager.FileExists(*ImageFilename))
        {
            OutError = FString::Printf(TEXT("Image does not exist: %s"), *ImageFilename);
            return;
        }

        const int64 ImageFileSizeBytes = FileManager.FileSize(*ImageFilename);
        check(ImageFileSizeBytes != INDEX_NONE);

        // check filesize
        if (ImageFileSizeBytes > MAX_FILESIZE_BYTES)
        {
            OutError = FString::Printf(TEXT("Image filesize > %d MBs): %s"), MAX_FILESIZE_BYTES, *ImageFilename);
            return;
        }
        
        TArray<uint8> ImageBuffer;
        {
            QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageUtils_ImportFileAsTexture_LoadFileToArray);
            if (!FFileHelper::LoadFileToArray(ImageBuffer, *ImageFilename))
            {
                OutError = FString::Printf(TEXT("Image I/O error: %s"), *ImageFilename);
                return;
            }
            ImageBuffer.Add(0);
        }

        const FFileStatData ImageFileStatData = FileManager.GetStatData(*ImageFilename);
        OutImage.ModificationTime = FMath::Max(ImageFileStatData.CreationTime, ImageFileStatData.ModificationTime);

        if (!ImportBufferAsImage(ImageBuffer.GetData(), ImageBuffer.Num() - 1, OutImage, OutError))
        {
            return;
        }
    }

    UTexture2D* CreateTexture(const FString& ImageFilename, const FRuntimeImageData& ImageData)
    {
        check(IsInGameThread());

        const FString& BaseFilename = FPaths::GetBaseFilename(ImageFilename);

        UTexture2D* NewTexture = NewObject<UTexture2D>(
            GetTransientPackage(), 
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), *BaseFilename),
            RF_Public | RF_Transient
        );
        NewTexture->NeverStream = true;
        NewTexture->SRGB = ImageData.SRGB;

        {
            QUICK_SCOPE_CYCLE_COUNTER(STAT_RuntimeImageReader_ImportFileAsTexture_NewTexture);

            check(IsValid(NewTexture));

            FTexturePlatformData* PlatformData = new FTexturePlatformData();

#if ENGINE_MAJOR_VERSION < 5
            NewTexture->PlatformData = PlatformData;
#else
            NewTexture->SetPlatformData(PlatformData);
#endif

            PlatformData->SizeX = ImageData.SizeX;
            PlatformData->SizeY = ImageData.SizeY;
            PlatformData->PixelFormat = ImageData.PixelFormat;

            FTexture2DMipMap* Mip = new FTexture2DMipMap();
            PlatformData->Mips.Add(Mip);
            Mip->SizeX = ImageData.SizeX;
            Mip->SizeY = ImageData.SizeY;

            /*const uint32 MipBytes = Mip->SizeX * Mip->SizeY * GPixelFormats[PixelFormat].BlockBytes;
            {
                Mip->BulkData.Lock(LOCK_READ_WRITE);

                void* TextureData = Mip->BulkData.Realloc(MipBytes);

                static TArray<uint8> DummyBytes;
                DummyBytes.SetNum(MipBytes);

                FMemory::Memcpy(TextureData, DummyBytes.GetData(), MipBytes);

                Mip->BulkData.Unlock();
            }*/

            // NewTexture->UpdateResource();
        }

        return NewTexture;
    }
}
