#include <QCoreApplication>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include "diskio.h"

static qint64 ceilTo(qint64 v, qint64 a) { return (a>0)? ((v + a - 1) / a) * a : v; }
static qint64 floorTo(qint64 v, qint64 a) { return (a>0)? (v - (v % a)) : v; }


int logicExec(){
    QTextStream out(stdout), err(stderr);

    out << "=== RawWriter ===\n";

    auto disks = DiskIO::enumerate(err);
    if (disks.isEmpty()) return 1;

    out << "Найдены диски:\n";
    for (int i=0;i<disks.size();++i) {
        const auto &d = disks[i];
        out << " [" << i << "] " << d.path
            << " | " << d.model
            << " | " << DiskIO::humanSize(d.size)
            << " | L=" << d.logicalSector << " P=" << d.physicalSector
            << (d.removable ? " | removable" : "")
            << "\n";
    }

    out << "\nВведите индекс диска для работы: " << Qt::flush;
    bool ok=false;
    QString sIdx = QTextStream(stdin).readLine().trimmed();
    int idx = sIdx.toInt(&ok);
    if (!ok || idx < 0 || idx >= disks.size()) {
        err << "Некорректный индекс.\n";
        return 1;
    }
    const DiskInfo target = disks[idx];

    out << "Режим (write/read) [w/r]: " << Qt::flush;
    QString mode = QTextStream(stdin).readLine().trimmed().toLower();
    if (mode.isEmpty() || (mode!="w" && mode!="r" && mode!="write" && mode!="read")) {
        err << "Некорректный режим.\n";
        return 1;
    }
    bool isWrite = (mode=="w" || mode=="write");

    out << "Размер блока, байт [1048576]: " << Qt::flush;
    QString bsStr = QTextStream(stdin).readLine().trimmed();
    qint64 blockSize = bsStr.isEmpty() ? 1048576 : bsStr.toLongLong(&ok);
    if (!ok || blockSize <= 0) { err << "Некорректный размер блока.\n"; return 1; }

    out << "Смещение на устройстве, байт [0]: " << Qt::flush;
    QString offStr = QTextStream(stdin).readLine().trimmed();
    qint64 devOffset = offStr.isEmpty() ? 0 : offStr.toLongLong(&ok);
    if (!ok || devOffset < 0) { err << "Некорректное смещение.\n"; return 1; }

    out << "Максимальный объём, байт (пусто = весь источник/устройство): " << Qt::flush;
    QString limStr = QTextStream(stdin).readLine().trimmed();
    qint64 limit = -1;
    if (!limStr.isEmpty()) { limit = limStr.toLongLong(&ok); if (!ok || limit <= 0) { err<<"Некорректный лимит.\n"; return 1; } }

    const qint64 sector = std::max<qint64>(target.logicalSector, 512);
    if ((devOffset % sector) != 0) {
        err << "Смещение должно быть кратно размеру логического сектора (" << sector << " байт). Сейчас: " << devOffset << ".\n";
        return 1;
    }
    if ((blockSize % sector) != 0) {
        err << "Размер блока должен быть кратен " << sector << " байт. Сейчас: " << blockSize << ".\n";
        return 1;
    }

    if (isWrite) {
        out << "Путь к входному файлу-образу: " << Qt::flush;
        QString inPath = QTextStream(stdin).readLine().trimmed();
        if (inPath.isEmpty() || !QFileInfo::exists(inPath)) { err << "Входной файл не найден.\n"; return 1; }

        out << "\nВНИМАНИЕ! Будет перезаписано устройство: " << target.path
            << "\nМодель: " << target.model
            << "\nРазмер: " << DiskIO::humanSize(target.size)
            << "\nСектор: логический " << target.logicalSector << ", физический " << target.physicalSector
            << "\nРежим: WRITE"
            << "\nФайл: " << inPath
            << "\nСмещение: " << devOffset
            << "\nЛимит: " << (limit<0?QString("весь файл"):QString::number(limit))
            << "\nПродолжить? (yes/NO): " << Qt::flush;
        QString conf = QTextStream(stdin).readLine().trimmed().toLower();
        if (conf != "yes") { out << "Отменено пользователем.\n"; return 0; }

        QFile inFile(inPath);
        if (!inFile.open(QIODevice::ReadOnly)) { err << "Не открыть входной файл: " << inFile.errorString() << "\n"; return 1; }

        QFile dev;
        QString diag;
        quint32 l=target.logicalSector, p=target.physicalSector;
        if (!DiskIO::openWrite(target.path, dev, diag, l, p)) {
            err << "Не открыть устройство для записи. " << diag << "\n";
#ifdef Q_OS_WIN
            err << "Подсказки: Админ-права, размонтировать том (mountvol/diskpart), закрыть Проводник/антивирус, выбрать именно \\\\.\\PhysicalDriveN.\n";
#else
            err << "Подсказки: sudo/root; umount всех разделов устройства; убедитесь, что это весь диск (/dev/sdX).\n";
#endif
            return 1;
        }
        if (devOffset>0 && !dev.seek(devOffset)) { err << "Не удалось перейти на указанное смещение устройства.\n"; return 1; }

        qint64 srcSize = inFile.size();
        qint64 base = (limit < 0) ? srcSize : std::min<qint64>(limit, srcSize);
        qint64 targetBytes = ceilTo(base, sector);
        if (targetBytes == 0) targetBytes = sector;

        bool okCopy = DiskIO::copyAlignedWithPadding(inFile, dev, targetBytes, blockSize, sector, true, out, err);
        return okCopy ? 0 : 2;

    } else {
        out << "Путь для выходного файла (куда читать с устройства): " << Qt::flush;
        QString outPath = QTextStream(stdin).readLine().trimmed();
        if (outPath.isEmpty()) { err << "Не указан путь выходного файла.\n"; return 1; }

        out << "\nБудет СЧИТАНО с устройства: " << target.path
            << "\nМодель: " << target.model
            << "\nРазмер: " << DiskIO::humanSize(target.size)
            << "\nСектор: логический " << target.logicalSector << ", физический " << target.physicalSector
            << "\nРежим: READ"
            << "\nФайл: " << outPath
            << "\nСмещение: " << devOffset
            << "\nЛимит: " << (limit<0?QString("до конца устройства"):QString::number(limit))
            << "\nПродолжить? (yes/NO): " << Qt::flush;
        QString conf = QTextStream(stdin).readLine().trimmed().toLower();
        if (conf != "yes") { out << "Отменено пользователем.\n"; return 0; }

        QFile dev;
        QString diag;
        quint32 l=target.logicalSector, p=target.physicalSector;
        if (!DiskIO::openRead(target.path, dev, diag, l, p)) {
            err << "Не открыть устройство для чтения. " << diag << "\n";
#ifdef Q_OS_WIN
            err << "Подсказки: Админ-права, закрыть Проводник/антивирус, выбрать именно \\\\.\\PhysicalDriveN.\n";
#else
            err << "Подсказки: sudo/root; umount всех разделов устройства; убедитесь, что это весь диск (/dev/sdX).\n";
#endif
            return 1;
        }
        if (devOffset>0 && !dev.seek(devOffset)) { err << "Не удалось перейти на указанное смещение устройства.\n"; return 1; }

        QFile outFile(outPath);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) { err << "Не открыть выходной файл: " << outFile.errorString() << "\n"; return 1; }

        qint64 toRead = limit;
        if (toRead > 0) {
            toRead = floorTo(toRead, sector);
            if (toRead == 0) { err << "Лимит меньше размера сектора. Увеличьте лимит.\n"; return 1; }
        }

        bool okCopy = DiskIO::copyAlignedWithPadding(dev, outFile, (toRead>0? toRead : 0x7fffffffffffffffLL), blockSize, sector, false, out, err);
        return okCopy ? 0 : 2;
    }
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("rawwriter");
    QCoreApplication::setApplicationVersion("6.0");
#ifdef Q_OS_WIN
system("chcp 65001");
#endif
auto retVal=logicExec();
QTextStream(stdout)<<"\n\nНажмите Enter для завершения...\n";
QTextStream(stdin).readLine();
return retVal;
}
