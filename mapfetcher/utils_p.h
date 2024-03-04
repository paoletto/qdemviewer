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

#ifndef UTILS_P_H
#define UTILS_P_H

#include <QList>
#include <QString>

struct URLTemplate {
    QString hostWildcarded;
    QList<QString> hostAlternatives;
    QList<QString> alternatives;
};

URLTemplate extractTemplates(QString urlTemplate);

#endif
