#pragma once
#include <QString>
#include <QVector>
#include <QFile>
#include <QTextStream>

struct DiskInfo {
    QString path;
    QString model;
    quint64 size = 0;
    bool removable = false;
    quint32 logicalSector = 512;
    quint32 physicalSector = 512;
};

class DiskIO {
public:
    static QVector<DiskInfo> enumerate(QTextStream &err);
    static QString humanSize(quint64 b);

    // Открытие устройства для записи/чтения (raw). На Windows — CreateFileW + wrap в QFile.
    static bool openWrite(const QString &devicePath, QFile &outFile, QString &diag, quint32 &logicalSector, quint32 &physicalSector);
    static bool openRead (const QString &devicePath, QFile &outFile, QString &diag, quint32 &logicalSector, quint32 &physicalSector);

    // Принудительный сброс буферов на устройство
    static bool flushToDisk(QFile &f);

    // Копирование с выравниванием и дописыванием нулями (на write-пути)
    static bool copyAlignedWithPadding(QFile &src, QFile &dst, qint64 totalTarget, qint64 blockSize, qint64 sectorAlign, bool padUp, QTextStream &out, QTextStream &err);
};
