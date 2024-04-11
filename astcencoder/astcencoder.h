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
#include <QBuffer>
#include <QByteArray>

#include <QThreadPool>
#include <QThread>

#include <QStandardPaths>
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>

#include <iostream>
#include <string>
#include <cstdlib>
#include <vector>
#include <map>
#include <private/qtexturefiledata_p.h>
#include <private/qtexturefilereader_p.h>

#include "astccache.h"
#include "astcenc.h"

// to use a single astc context
class ASTCEncoder
{
public:
    static ASTCEncoder& instance();

    QTextureFileData compress(QImage ima);

    static QImage halve(const QImage &src);

    static int blockSize();

    static QTextureFileData fromCached(QByteArray &cached);

    void generateMips(const QImage &ima, std::vector<QTextureFileData> &out, QByteArray md5);

    bool isCached(const QByteArray &md5);

private:
    ASTCEncoder();

    ~ASTCEncoder();

    struct astcenc_context_deleter {
        static inline void cleanup(astcenc_context *c)
        {
            if (c)
                astcenc_context_free(c);
            c = nullptr;
        }
    };

    QScopedPointer<astcenc_context, astcenc_context_deleter> m_ctx;
    astcenc_swizzle swizzle;
    astcenc_config config;
    QString m_cacheDirPath;
    ASTCCache m_tileCache;

    static const astcenc_profile profile = ASTCENC_PRF_LDR;

//    static const float ASTCENC_PRE_FASTEST = 0.0f;
//    static const float ASTCENC_PRE_FAST = 10.0f;
//    static const float ASTCENC_PRE_MEDIUM = 60.0f;
//    static const float ASTCENC_PRE_THOROUGH = 98.0f;
//    static const float ASTCENC_PRE_VERYTHOROUGH = 99.0f;
//    static const float ASTCENC_PRE_EXHAUSTIVE = 100.0f;

    constexpr static const float quality = 85.0f;

    static const unsigned int thread_count = 1;
    static const unsigned int block_x = 8;
    static const unsigned int block_y = 8;
//    static const unsigned int block_x = 4;
//    static const unsigned int block_y = 4;
//    static const unsigned int block_x = 6;
//    static const unsigned int block_y = 6;
    static const unsigned int block_z = 1;
    static const uint32_t ASTC_MAGIC_ID = 0x5CA1AB13;

public:
    ASTCEncoder(ASTCEncoder const&)            = delete;
    void operator=(ASTCEncoder const&)         = delete;
};


#endif
