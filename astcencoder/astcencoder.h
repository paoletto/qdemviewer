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
        BlockSize12x12 = 12,
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


    unsigned int block_x = 8;
    unsigned int block_y = 8;
    float quality = 85.0f;
};

// to use a single astc context
struct ASTCEncoderPrivate;
class ASTCEncoder
{
public:


    static ASTCEncoder& instance(ASTCEncoderConfig::BlockSize bs = ASTCEncoderConfig::BlockSize8x8, float quality = 85.f);

    static QImage halve(const QImage &src);

    static QTextureFileData fromCached(QByteArray &cached);

    void generateMips(const QImage &ima,
                      quint64 x,
                      quint64 y,
                      quint64 z,
                      std::vector<QTextureFileData> &out, QByteArray md5);
    static void generateMips(QImage ima, std::vector<QImage> &out);

    bool isCached(const QByteArray &md5);

protected:
    QTextureFileData compress(QImage ima);

private:
    ASTCEncoder(ASTCEncoderConfig c);

    ~ASTCEncoder();

    QScopedPointer<ASTCEncoderPrivate> d;

public:
    ASTCEncoder(ASTCEncoder const&)            = delete;
    void operator=(ASTCEncoder const&)         = delete;
};


#endif
