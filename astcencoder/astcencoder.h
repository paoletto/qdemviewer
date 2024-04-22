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



// to use a single astc context
struct ASTCEncoderPrivate;
class ASTCEncoder
{
public:
    static ASTCEncoder& instance();

    static QImage halve(const QImage &src);

    static int blockSize();

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
    ASTCEncoder();

    ~ASTCEncoder();

    QScopedPointer<ASTCEncoderPrivate> d;

public:
    ASTCEncoder(ASTCEncoder const&)            = delete;
    void operator=(ASTCEncoder const&)         = delete;
};


#endif
