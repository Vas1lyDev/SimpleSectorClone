#include "diskio.h"
#include <QDir>
#include <QElapsedTimer>
#include <QByteArray>
#include <QFileInfo>
#include <algorithm>
#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <winioctl.h>
#endif

#ifdef Q_OS_UNIX
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <errno.h>
#  include <string.h>
#endif

QString DiskIO::humanSize(quint64 b) {
    const char* units[] = {"B","KiB","MiB","GiB","TiB"};
    int i=0; double v=b;
    while (v>=1024.0 && i<4) { v/=1024.0; ++i; }
    return QString::number(v, 'f', (i==0?0:(v<10?2:1))) + " " + units[i];
}

bool DiskIO::flushToDisk(QFile &f) {
    f.flush();
#ifdef Q_OS_WIN
    int fd = f.handle();
    if (fd >= 0) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h != INVALID_HANDLE_VALUE) {
            return FlushFileBuffers(h);
        }
    }
    return false;
#else
    int fd = f.handle();
    if (fd >= 0) {
        return (::fsync(fd) == 0);
    }
    return false;
#endif
}

#ifdef Q_OS_UNIX
static quint64 readFileULongLong(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    QByteArray s = f.readAll().trimmed();
    bool ok=false;
    quint64 v = s.toULongLong(&ok);
    return ok ? v : 0;
}
#endif

#ifdef Q_OS_WIN
static bool getDriveSize(HANDLE h, quint64 &sizeOut) {
    GET_LENGTH_INFORMATION lenInfo{};
    DWORD ret=0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                        nullptr, 0, &lenInfo, sizeof(lenInfo),
                        &ret, nullptr)) {
        sizeOut = static_cast<quint64>(lenInfo.Length.QuadPart);
        return true;
    }
    return false;
}

static void getAlignment(HANDLE h, quint32 &logical, quint32 &physical) {
    logical = 512; physical = 512;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageAccessAlignmentProperty;
    q.QueryType  = PropertyStandardQuery;
    BYTE buf[512]{}; DWORD ret=0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                        buf, sizeof(buf), &ret, nullptr)) {
        struct STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR {
            DWORD Version;
            DWORD Size;
            DWORD BytesPerCacheLine;
            DWORD BytesOffsetForCacheAlignment;
            DWORD BytesPerLogicalSector;
            DWORD BytesPerPhysicalSector;
            DWORD BytesOffsetForSectorAlignment;
        };
        auto *d = reinterpret_cast<STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(buf);
        if (d->BytesPerLogicalSector) logical = d->BytesPerLogicalSector;
        if (d->BytesPerPhysicalSector) physical = d->BytesPerPhysicalSector;
    } else {
        DISK_GEOMETRY_EX geom{}; ret=0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geom, sizeof(geom), &ret, nullptr)) {
            logical = physical = geom.Geometry.BytesPerSector ? geom.Geometry.BytesPerSector : 512;
        }
    }
}

static QString sysErrorMessage(DWORD code) {
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&buf, 0, nullptr);
    QString msg = QString("Windows ошибка %1: %2").arg(code).arg(len? QString::fromWCharArray(buf).trimmed() : QStringLiteral("Неизвестная ошибка"));
    if (buf) LocalFree(buf);
    return msg;
}

static QString getDriveModel(HANDLE h) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    BYTE buffer[1024]{};
    DWORD ret=0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query),
                        buffer, sizeof(buffer), &ret, nullptr)) {
        auto desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        if (desc->ProductIdOffset) {
            const char* p = reinterpret_cast<const char*>(buffer) + desc->ProductIdOffset;
            return QString::fromLatin1(p);
        }
    }
    return QString();
}
#endif

QVector<DiskInfo> DiskIO::enumerate(QTextStream &err) {
    QVector<DiskInfo> out;
#ifdef Q_OS_UNIX
    QDir sysBlock("/sys/block");
    const QStringList entries = sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        if (name.startsWith("loop") || name.startsWith("ram") || name.startsWith("sr")) continue;
        const QString devPath = "/dev/" + name;
        struct stat st{};
        if (::stat(devPath.toLocal8Bit().constData(), &st) != 0 || !S_ISBLK(st.st_mode))
            continue;

        quint64 sectors = readFileULongLong("/sys/block/" + name + "/size");
        quint64 size = sectors * 512ull;
        bool removable = readFileULongLong("/sys/block/" + name + "/removable") == 1;
        QString model;
        QFile m("/sys/block/" + name + "/device/model");
        if (m.open(QIODevice::ReadOnly))
            model = QString::fromLatin1(m.readAll().trimmed());
        if (model.isEmpty()) model = "Unknown";

        quint32 logSz = 512, phySz = 512;
        QFile ql("/sys/block/" + name + "/queue/logical_block_size");
        if (ql.open(QIODevice::ReadOnly)) logSz = ql.readAll().trimmed().toUInt();
        QFile qp("/sys/block/" + name + "/queue/physical_block_size");
        if (qp.open(QIODevice::ReadOnly)) phySz = qp.readAll().trimmed().toUInt();

        DiskInfo d;
        d.path = devPath;
        d.model = model;
        d.size = size;
        d.removable = removable;
        d.logicalSector = logSz ? logSz : 512;
        d.physicalSector = phySz ? phySz : d.logicalSector;
        out.push_back(d);
    }
#elif defined(Q_OS_WIN)
    for (int n=0; n<32; ++n) {
        QString path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(n);
        HANDLE h = CreateFileW((LPCWSTR)path.utf16(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        quint64 size=0;
        bool ok = getDriveSize(h, size);
        QString model = getDriveModel(h);
        quint32 logSz=512, phySz=512;
        getAlignment(h, logSz, phySz);
        CloseHandle(h);
        if (!ok) continue;

        DiskInfo d;
        d.path = path;
        d.model = model.isEmpty() ? "PhysicalDrive" + QString::number(n) : model;
        d.size = size;
        d.removable = false;
        d.logicalSector = logSz;
        d.physicalSector = phySz;
        out.push_back(d);
    }
#else
    Q_UNUSED(err);
#endif
    if (out.isEmpty()) {
        err << "Не удалось обнаружить ни одного физического диска.\n";
    }
    return out;
}

bool DiskIO::openWrite(const QString &devicePath, QFile &outFile, QString &diag, quint32 &logicalSector, quint32 &physicalSector) {
#ifdef Q_OS_WIN
    HANDLE h = CreateFileW((LPCWSTR)devicePath.utf16(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        diag = sysErrorMessage(GetLastError());
        return false;
    }
    getAlignment(h, logicalSector, physicalSector);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        diag = "CRT _open_osfhandle() вернул ошибку.";
        return false;
    }
    if (!outFile.open(fd, QIODevice::WriteOnly | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
        diag = QString("QFile::open(fd) не удалось: %1").arg(outFile.errorString());
        return false;
    }
    return true;
#else
    Q_UNUSED(physicalSector);
    Q_UNUSED(logicalSector);
    outFile.setFileName(devicePath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        diag = outFile.errorString();
        return false;
    }
    return true;
#endif
}

bool DiskIO::openRead(const QString &devicePath, QFile &outFile, QString &diag, quint32 &logicalSector, quint32 &physicalSector) {
#ifdef Q_OS_WIN
    HANDLE h = CreateFileW((LPCWSTR)devicePath.utf16(),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        diag = sysErrorMessage(GetLastError());
        return false;
    }
    getAlignment(h, logicalSector, physicalSector);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        diag = "CRT _open_osfhandle() вернул ошибку.";
        return false;
    }
    if (!outFile.open(fd, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
        diag = QString("QFile::open(fd) не удалось: %1").arg(outFile.errorString());
        return false;
    }
    return true;
#else
    Q_UNUSED(physicalSector);
    Q_UNUSED(logicalSector);
    outFile.setFileName(devicePath);
    if (!outFile.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        diag = outFile.errorString();
        return false;
    }
    return true;
#endif
}

bool DiskIO::copyAlignedWithPadding(QFile &src, QFile &dst, qint64 totalTarget, qint64 blockSize, qint64 sectorAlign, bool padUp, QTextStream &out, QTextStream &err) {
    QByteArray buf; buf.resize(int(blockSize));
    QElapsedTimer t; t.start();
    qint64 done=0;

    while (done < totalTarget) {
        qint64 remaining = totalTarget - done;
        qint64 want = std::min<qint64>(buf.size(), remaining);

        qint64 rd = src.read(buf.data(), want);
        if (rd < 0) { err << "\nОшибка чтения источника.\n"; return false; }
        if (rd == 0) {
            if (padUp) {
                std::memset(buf.data(), 0, int(want));
                qint64 off=0;
                while (off < want) {
                    qint64 wr = dst.write(buf.constData()+off, want-off);
                    if (wr <= 0) { err << "\nОшибка записи при добивке нулями: " << dst.errorString() << "\n"; return false; }
                    off += wr;
                }
                done += want;
            } else {
                break;
            }
        } else {
            if (rd < want && padUp) {
                std::memset(buf.data()+rd, 0, int(want - rd));
                qint64 off=0;
                while (off < want) {
                    qint64 wr = dst.write(buf.constData()+off, want-off);
                    if (wr <= 0) { err << "\nОшибка записи при копировании (добивка): " << dst.errorString() << "\n"; return false; }
                    off += wr;
                }
                done += want;
            } else {
                qint64 off=0;
                while (off < rd) {
                    qint64 wr = dst.write(buf.constData()+off, rd-off);
                    if (wr <= 0) { err << "\nОшибка записи при копировании: " << dst.errorString() << "\n"; return false; }
                    off += wr;
                }
                done += rd;
            }
        }

        if ((done % (blockSize*32)) == 0 || done == totalTarget) {
            double secs = t.elapsed()/1000.0;
            double mb = done/1024.0/1024.0;
            double spd = secs>0 ? mb/secs : 0.0;
            out << "\rПередано: " << DiskIO::humanSize(done)
                << " / " << DiskIO::humanSize(totalTarget)
                << "  (" << QString::number(spd, 'f', 2) << " MiB/s)" << Qt::flush;
        }
    }

    if (!DiskIO::flushToDisk(dst)) {
        err << "\nПредупреждение: не удалось гарантированно сбросить буферы на устройство.\n";
    }
    out << "\nГотово. Итого: " << DiskIO::humanSize(done) << "\n";
    return true;
}
