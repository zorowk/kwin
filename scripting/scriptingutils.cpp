/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2012 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "scriptingutils.h"

#include <QDBusObjectPath>
#include <QDBusSignature>

namespace KWin
{
bool validateParameters(QScriptContext *context, int min, int max)
{
    if (context->argumentCount() < min || context->argumentCount() > max) {
        context->throwError(QScriptContext::SyntaxError,
                            i18nc("syntax error in KWin script", "Invalid number of arguments"));
        return false;
    }
    return true;
}

template<>
bool validateArgumentType<QVariant>(QScriptContext *context, int argument)
{
    const bool result =context->argument(argument).toVariant().isValid();
    if (!result) {
        context->throwError(QScriptContext::TypeError,
            i18nc("KWin Scripting function received incorrect value for an expected type",
                  "%1 is not a variant type", context->argument(argument).toString()));
    }
    return result;
}

QVariant dbusToVariant(const QVariant &variant)
{
    if (variant.canConvert<QDBusArgument>()) {
        const QDBusArgument argument = variant.value<QDBusArgument>();
        switch (argument.currentType()) {
        case QDBusArgument::BasicType:
            return dbusToVariant(argument.asVariant());
        case QDBusArgument::VariantType:
            return dbusToVariant(argument.asVariant().value<QDBusVariant>().variant());
        case QDBusArgument::ArrayType: {
            QVariantList array;
            argument.beginArray();
            while (!argument.atEnd()) {
                const QVariant value = argument.asVariant();
                array.append(dbusToVariant(value));
            }
            argument.endArray();
            return array; }
        case QDBusArgument::StructureType: {
            QVariantList structure;
            argument.beginStructure();
            while (!argument.atEnd()) {
                const QVariant value = argument.asVariant();
                structure.append(dbusToVariant(value));
            }
            argument.endStructure();
            return structure; }
        case QDBusArgument::MapType: {
            QVariantMap map;
            argument.beginMap();
            while (!argument.atEnd()) {
                argument.beginMapEntry();
                const QVariant key = argument.asVariant();
                const QVariant value = argument.asVariant();
                argument.endMapEntry();
                map.insert(key.toString(), dbusToVariant(value));
            }
            argument.endMap();
            return map; }
        default:
            qCWarning(KWIN_SCRIPTING) << "Couldn't unwrap QDBusArgument of type" << argument.currentType();
            return variant;
        }
    } else if (variant.canConvert<QDBusObjectPath>()) {
        return variant.value<QDBusObjectPath>().path();
    } else if (variant.canConvert<QDBusSignature>()) {
        return variant.value<QDBusSignature>().signature();
    } else if (variant.canConvert<QDBusVariant>()) {
        return dbusToVariant(variant.value<QDBusVariant>().variant());
    }

    return variant;
}

}
