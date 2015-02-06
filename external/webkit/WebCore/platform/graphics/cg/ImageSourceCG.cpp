

#include "config.h"
#include "ImageSource.h"

#if PLATFORM(CG)
#include "ImageSourceCG.h"

#include "IntSize.h"
#include "MIMETypeRegistry.h"
#include "SharedBuffer.h"
#include <ApplicationServices/ApplicationServices.h>
#include <wtf/UnusedParam.h>

using namespace std;

namespace WebCore {

static const CFStringRef kCGImageSourceShouldPreferRGB32 = CFSTR("kCGImageSourceShouldPreferRGB32");

#if !PLATFORM(MAC)
size_t sharedBufferGetBytesAtPosition(void* info, void* buffer, off_t position, size_t count)
{
    SharedBuffer* sharedBuffer = static_cast<SharedBuffer*>(info);
    size_t sourceSize = sharedBuffer->size();
    if (position >= sourceSize)
        return 0;

    const char* source = sharedBuffer->data() + position;
    size_t amount = min<size_t>(count, sourceSize - position);
    memcpy(buffer, source, amount);
    return amount;
}

void sharedBufferRelease(void* info)
{
    SharedBuffer* sharedBuffer = static_cast<SharedBuffer*>(info);
    sharedBuffer->deref();
}
#endif

ImageSource::ImageSource()
    : m_decoder(0)
{
}

ImageSource::~ImageSource()
{
    clear(true);
}

void ImageSource::clear(bool destroyAllFrames, size_t, SharedBuffer* data, bool allDataReceived)
{
#if PLATFORM(MAC) && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
    // Recent versions of ImageIO discard previously decoded image frames if the client
    // application no longer holds references to them, so there's no need to throw away
    // the decoder unless we're explicitly asked to destroy all of the frames.

    if (!destroyAllFrames)
        return;
#else
    // Older versions of ImageIO hold references to previously decoded image frames.
    // There is no API to selectively release some of the frames it is holding, and
    // if we don't release the frames we use too much memory on large images.
    // Destroying the decoder is the only way to release previous frames.

    UNUSED_PARAM(destroyAllFrames);
#endif

    if (m_decoder) {
        CFRelease(m_decoder);
        m_decoder = 0;
    }
    if (data)
        setData(data, allDataReceived);
}

static CFDictionaryRef imageSourceOptions()
{
    static CFDictionaryRef options;

    if (!options) {
        const unsigned numOptions = 2;
        const void* keys[numOptions] = { kCGImageSourceShouldCache, kCGImageSourceShouldPreferRGB32 };
        const void* values[numOptions] = { kCFBooleanTrue, kCFBooleanTrue };
        options = CFDictionaryCreate(NULL, keys, values, numOptions, 
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    return options;
}

bool ImageSource::initialized() const
{
    return m_decoder;
}

void ImageSource::setData(SharedBuffer* data, bool allDataReceived)
{
    if (!m_decoder)
        m_decoder = CGImageSourceCreateIncremental(NULL);
#if PLATFORM(MAC)
    // On Mac the NSData inside the SharedBuffer can be secretly appended to without the SharedBuffer's knowledge.  We use SharedBuffer's ability
    // to wrap itself inside CFData to get around this, ensuring that ImageIO is really looking at the SharedBuffer.
    RetainPtr<CFDataRef> cfData(AdoptCF, data->createCFData());
    CGImageSourceUpdateData(m_decoder, cfData.get(), allDataReceived);
#else
    // Create a CGDataProvider to wrap the SharedBuffer.
    data->ref();
    // We use the GetBytesAtPosition callback rather than the GetBytePointer one because SharedBuffer
    // does not provide a way to lock down the byte pointer and guarantee that it won't move, which
    // is a requirement for using the GetBytePointer callback.
    CGDataProviderDirectCallbacks providerCallbacks = { 0, 0, 0, sharedBufferGetBytesAtPosition, sharedBufferRelease };
    RetainPtr<CGDataProviderRef> dataProvider(AdoptCF, CGDataProviderCreateDirect(data, data->size(), &providerCallbacks));
    CGImageSourceUpdateDataProvider(m_decoder, dataProvider.get(), allDataReceived);
#endif
}

String ImageSource::filenameExtension() const
{
    if (!m_decoder)
        return String();
    CFStringRef imageSourceType = CGImageSourceGetType(m_decoder);
    return WebCore::preferredExtensionForImageSourceType(imageSourceType);
}

bool ImageSource::isSizeAvailable()
{
    bool result = false;
    CGImageSourceStatus imageSourceStatus = CGImageSourceGetStatus(m_decoder);

    // Ragnaros yells: TOO SOON! You have awakened me TOO SOON, Executus!
    if (imageSourceStatus >= kCGImageStatusIncomplete) {
        RetainPtr<CFDictionaryRef> image0Properties(AdoptCF, CGImageSourceCopyPropertiesAtIndex(m_decoder, 0, imageSourceOptions()));
        if (image0Properties) {
            CFNumberRef widthNumber = (CFNumberRef)CFDictionaryGetValue(image0Properties.get(), kCGImagePropertyPixelWidth);
            CFNumberRef heightNumber = (CFNumberRef)CFDictionaryGetValue(image0Properties.get(), kCGImagePropertyPixelHeight);
            result = widthNumber && heightNumber;
        }
    }
    
    return result;
}

IntSize ImageSource::frameSizeAtIndex(size_t index) const
{
    IntSize result;
    RetainPtr<CFDictionaryRef> properties(AdoptCF, CGImageSourceCopyPropertiesAtIndex(m_decoder, index, imageSourceOptions()));
    if (properties) {
        int w = 0, h = 0;
        CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelWidth);
        if (num)
            CFNumberGetValue(num, kCFNumberIntType, &w);
        num = (CFNumberRef)CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelHeight);
        if (num)
            CFNumberGetValue(num, kCFNumberIntType, &h);
        result = IntSize(w, h);
    }
    return result;
}

IntSize ImageSource::size() const
{
    return frameSizeAtIndex(0);
}

int ImageSource::repetitionCount()
{
    int result = cAnimationLoopOnce; // No property means loop once.
    if (!initialized())
        return result;

    // A property with value 0 means loop forever.
    RetainPtr<CFDictionaryRef> properties(AdoptCF, CGImageSourceCopyProperties(m_decoder, imageSourceOptions()));
    if (properties) {
        CFDictionaryRef gifProperties = (CFDictionaryRef)CFDictionaryGetValue(properties.get(), kCGImagePropertyGIFDictionary);
        if (gifProperties) {
            CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(gifProperties, kCGImagePropertyGIFLoopCount);
            if (num)
                CFNumberGetValue(num, kCFNumberIntType, &result);
        } else
            result = cAnimationNone; // Turns out we're not a GIF after all, so we don't animate.
    }
    
    return result;
}

size_t ImageSource::frameCount() const
{
    return m_decoder ? CGImageSourceGetCount(m_decoder) : 0;
}

CGImageRef ImageSource::createFrameAtIndex(size_t index)
{
    if (!initialized())
        return 0;

    RetainPtr<CGImageRef> image(AdoptCF, CGImageSourceCreateImageAtIndex(m_decoder, index, imageSourceOptions()));
    CFStringRef imageUTI = CGImageSourceGetType(m_decoder);
    static const CFStringRef xbmUTI = CFSTR("public.xbitmap-image");
    if (!imageUTI || !CFEqual(imageUTI, xbmUTI))
        return image.releaseRef();
    
    // If it is an xbm image, mask out all the white areas to render them transparent.
    const CGFloat maskingColors[6] = {255, 255,  255, 255, 255, 255};
    RetainPtr<CGImageRef> maskedImage(AdoptCF, CGImageCreateWithMaskingColors(image.get(), maskingColors));
    if (!maskedImage)
        return image.releaseRef();

    return maskedImage.releaseRef();
}

bool ImageSource::frameIsCompleteAtIndex(size_t index)
{
    return CGImageSourceGetStatusAtIndex(m_decoder, index) == kCGImageStatusComplete;
}

float ImageSource::frameDurationAtIndex(size_t index)
{
    if (!initialized())
        return 0;

    float duration = 0;
    RetainPtr<CFDictionaryRef> properties(AdoptCF, CGImageSourceCopyPropertiesAtIndex(m_decoder, index, imageSourceOptions()));
    if (properties) {
        CFDictionaryRef typeProperties = (CFDictionaryRef)CFDictionaryGetValue(properties.get(), kCGImagePropertyGIFDictionary);
        if (typeProperties) {
            CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(typeProperties, kCGImagePropertyGIFDelayTime);
            if (num)
                CFNumberGetValue(num, kCFNumberFloatType, &duration);
        }
    }

    // Many annoying ads specify a 0 duration to make an image flash as quickly as possible.
    // We follow WinIE's behavior and use a duration of 100 ms for any frames that specify
    // a duration of <= 50 ms. See <http://bugs.webkit.org/show_bug.cgi?id=14413> or Radar 4051389 for more.
    if (duration < 0.051f)
        return 0.100f;
    return duration;
}

bool ImageSource::frameHasAlphaAtIndex(size_t)
{
    if (!m_decoder)
        return false;

    CFStringRef imageType = CGImageSourceGetType(m_decoder);

    // Return false if there is no image type or the image type is JPEG, because
    // JPEG does not support alpha transparency.
    if (!imageType || CFEqual(imageType, CFSTR("public.jpeg")))
        return false;

    // FIXME: Could return false for other non-transparent image formats.
    // FIXME: Could maybe return false for a GIF Frame if we have enough info in the GIF properties dictionary
    // to determine whether or not a transparent color was defined.
    return true;
}

}

#endif // PLATFORM(CG)