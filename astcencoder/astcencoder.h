/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** Commercial License Usage
** Licensees holding a valid commercial qdemviewer license may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement with the copyright holder. For licensing terms
** and conditions and further information contact the copyright holder.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3. The licenses are as published by
** the Free Software Foundation at https://www.gnu.org/licenses/gpl-3.0.html,
** with the exception that the use of this work for training artificial intelligence
** is prohibited for both commercial and non-commercial use.
**
****************************************************************************/

#ifndef ASTCENCODER_P_H
#define ASTCENCODER_P_H

#include <QImage>
#include <QByteArray>
#include <array>
#include <private/qtexturefiledata_p.h>

struct ASTCEncoderConfig {
    // check QOpenGLTexture::RGBA_ASTC_*
    // offy pixel bitrates are
    // 4x4 8bpp
    // 6x6 3.56 bpp
    // 8x8 2bpp
    // 10x10 1.28 bpp
    // 12x12 0.89 bpp
    enum BlockSize {
        BlockSize4x4 = 4,
        BlockSize6x6 = 6,
        BlockSize8x8 = 8,
        BlockSize10x10 = 10,
        BlockSize12x12 = 12
    };

    enum SwizzleComponent
    {
        /** @brief Select the red component. */
        ASTCENC_SWZ_R = 0,
        /** @brief Select the green component. */
        ASTCENC_SWZ_G = 1,
        /** @brief Select the blue component. */
        ASTCENC_SWZ_B = 2,
        /** @brief Select the alpha component. */
        ASTCENC_SWZ_A = 3,
        /** @brief Use a constant zero component. */
        ASTCENC_SWZ_0 = 4,
        /** @brief Use a constant one component. */
        ASTCENC_SWZ_1 = 5
    };

    enum ASTCProfile
    {
        /** @brief The LDR linear color profile. */
        ASTCENC_PRF_LDR = 1,
        /** @brief The HDR RGB with LDR alpha color profile. */
        ASTCENC_PRF_HDR_RGB_LDR_A = 2,
        /** @brief The HDR RGBA color profile. */
        ASTCENC_PRF_HDR = 3
    };

    static constexpr const float ASTCENC_PRE_FASTEST = 0.0f;
    static constexpr const float ASTCENC_PRE_FAST = 10.0f;
    static constexpr const float ASTCENC_PRE_MEDIUM = 60.0f;
    static constexpr const float ASTCENC_PRE_THOROUGH = 98.0f;
    static constexpr const float ASTCENC_PRE_VERYTHOROUGH = 99.0f;
    static constexpr const float ASTCENC_PRE_EXHAUSTIVE = 100.0f;

    bool operator<(const ASTCEncoderConfig& o) const {
        return block_x < o.block_x
                || (block_x == o.block_x && quality < o.quality);
    }
    using SwizzleConfig = std::array<int, 4>;
    using ChannelWeights = std::array<float, 4>;


    unsigned int block_x = 8;
    unsigned int block_y = 8;
    unsigned int profile = 1;
    float quality = 85.0f;   
    SwizzleConfig swizzle = { // QImage::Format_RGB32 == 0xffRRGGBB
                              ASTCENC_SWZ_B,
                              ASTCENC_SWZ_G,
                              ASTCENC_SWZ_R,
                              ASTCENC_SWZ_A
                            };

    ChannelWeights weights = {0.30f * 2.25f, // r,g,b,a
                              0.59f * 2.25f,
                              0.11f * 2.25f,
                              0.}; // Default to what ASTCENC_FLG_USE_PERCEPTUAL does

};

// to use a single astc context
struct ASTCEncoderPrivate;
class ASTCEncoder
{
public:
    static ASTCEncoder& instance(ASTCEncoderConfig::BlockSize bs = ASTCEncoderConfig::BlockSize8x8,
                                 float quality = 85.f,
                                 ASTCEncoderConfig::ASTCProfile profile = ASTCEncoderConfig::ASTCENC_PRF_LDR,
                                 ASTCEncoderConfig::SwizzleConfig swizzleConfig = { // QImage::Format_RGB32 == 0xffRRGGBB
                                    ASTCEncoderConfig::ASTCENC_SWZ_B,
                                    ASTCEncoderConfig::ASTCENC_SWZ_G,
                                    ASTCEncoderConfig::ASTCENC_SWZ_R,
                                    ASTCEncoderConfig::ASTCENC_SWZ_A
                                 },
                                 ASTCEncoderConfig::ChannelWeights channelWeights = {
                                    0.30f * 2.25f, // r,g,b,a
                                    0.59f * 2.25f,
                                    0.11f * 2.25f,
                                    0.}
                                 );

    static QImage halve(const QImage &src);

    static QTextureFileData fromCached(QByteArray &cached);

    void generateMips(const QImage &ima,
                      quint64 x,
                      quint64 y,
                      quint64 z,
                      std::vector<QTextureFileData> &out,
                      QByteArray md5);
    static void generateMips(QImage ima, std::vector<QImage> &out);
    void generateHDRMip(const std::vector<float> &ima,
                         QSize size,
                         quint64 x,
                         quint64 y,
                         quint64 z,
                         bool bordersComplete,
                         std::vector<QTextureFileData> &out,
                         QByteArray md5);

    bool isCached(const QByteArray &md5);

protected:
    QTextureFileData compress(QImage ima);
    QTextureFileData compress(const std::vector<float> &ima,
                              const QSize &size);

private:
    ASTCEncoder(ASTCEncoderConfig c);

    ~ASTCEncoder();

    QScopedPointer<ASTCEncoderPrivate> d;

public:
    ASTCEncoder(ASTCEncoder const&)            = delete;
    void operator=(ASTCEncoder const&)         = delete;
};


#endif
