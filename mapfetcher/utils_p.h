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

#ifndef UTILS_P_H
#define UTILS_P_H

#include <QList>
#include <QString>
#include <QImage>
#include <QOpenGLTexture>
#include <private/qtexturefiledata_p.h>

QByteArray md5QImage(const QImage &i);

struct URLTemplate {
    QString hostWildcarded;
    QList<QString> hostAlternatives;
    QList<QString> alternatives;
};

struct OpenGLTextureUtils {
    static std::vector<QImage> m_white256;
    static std::vector<QTextureFileData> m_white8x8ASTC;
    static std::vector<QTextureFileData> m_transparent8x8ASTC;
    static void init();

    static bool isFormatCompressed(GLint format);
    static quint64 fillSingleTextureUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                                 std::shared_ptr<QImage> &ima);
    static quint64 fillSingleTextureBPTC(QSharedPointer<QOpenGLTexture> &t,
                                         std::shared_ptr<QImage> &ima);
    static quint64 fillSingleTextureBPTC(QSharedPointer<QOpenGLTexture> &t,
                                         QSize texSize,
                                         std::vector<float> &data, float min = 0);
    static quint64 fillSingleTextureUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                                 const QSize &size,
                                                 std::vector<float> &data);
    static quint64 fillSingleTextureASTC(QSharedPointer<QOpenGLTexture> &t,
                                         std::vector<QTextureFileData> &mips);

    static quint64 fill2DArrayUncompressed(QSharedPointer<QOpenGLTexture> &t,
                                           std::shared_ptr<QImage> &ima,
                                           int layer,
                                           int layers);
    static quint64 fill2DArrayASTC(QSharedPointer<QOpenGLTexture> &t,
                                   std::vector<QTextureFileData> mips,
                                   int layer,
                                   int layers);
};

URLTemplate extractTemplates(QString urlTemplate);

#endif
