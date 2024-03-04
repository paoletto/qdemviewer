/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** This file is part of qdemviewer.
**
** This work is licensed under the terms of the Creative Commons
** Attribution-NonCommercial-ShareAlike 4.0 International License,
** with additional clauses. See the LICENSE file in the project root
** for full terms.
**
****************************************************************************/

#ifndef ASTCENCODER_P_H
#define ASTCENCODER_P_H

#include <QImage>
#include <QByteArray>
#include <private/qtexturefiledata_p.h>



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
