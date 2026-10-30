#include "BrandAssets.h"

#include "BinaryData.h"

namespace
{
    const juce::Drawable* getCachedLogo()
    {
        static const std::unique_ptr<juce::Drawable> logo =
            juce::Drawable::createFromImageData (BinaryData::LogoDark_svg,
                                                 BinaryData::LogoDark_svgSize);

        return logo.get();
    }
}

namespace BrandAssets
{
    const juce::Drawable* getLogo()
    {
        return getCachedLogo();
    }

    void drawLogo (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto* logo = getLogo();

        if (logo == nullptr || area.isEmpty())
            return;

        const auto width = static_cast<float> (logo->getWidth());
        const auto height = static_cast<float> (logo->getHeight());

        if (width <= 0.0f || height <= 0.0f)
            return;

        const auto scale = juce::jmin (area.getWidth() / width,
                                       area.getHeight() / height);

        const auto transform =
            juce::AffineTransform::scale (scale)
                .translated (area.getCentreX() - width * scale * 0.5f,
                             area.getCentreY() - height * scale * 0.5f);

        logo->draw (g, 1.0f, transform);
    }

    juce::Image createLogoImage (int size)
    {
        if (size <= 0)
            return {};

        juce::Image image (juce::Image::ARGB, size, size, true);
        juce::Graphics g (image);
        drawLogo (g, image.getBounds().toFloat());
        return image;
    }
}
