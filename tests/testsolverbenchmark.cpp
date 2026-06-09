/*
    SPDX-FileCopyrightText: 2025 Christian Kemper <ckemper@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later

    Ported from KStars Tests/fitsviewer/testsolverbenchmark.cpp.
    Compares MULTI_SCALES vs MULTI_DEPTHS across hint scenarios to validate
    that MULTI_AUTO selects the right algorithm for available hints.

    Requires pre-rendered FITS files (set FITS_DIR, default ~/Pictures)
    and astrometry index files (set INDEX_DIR, default kstars astrometry path).

    Run:  ./TestSolverBenchmark
    Env:  FITS_DIR=/path/to/fits  INDEX_DIR=/path/to/indexes
*/

#include <QTest>
#include <QElapsedTimer>
#include <QDir>

#include "stellarsolver.h"
#include "ssolverutils/fileio.h"

using SSolver::MULTI_SCALES;
using SSolver::MULTI_DEPTHS;

class TestSolverBenchmark : public QObject
{
        Q_OBJECT

    private:
        struct FieldInfo
        {
            QString filename;
            QString label;
            double ra;
            double dec;
            double pixscale;
        };

        QString m_fitsDir;
        QString m_indexDir;

        bool solveField(const FieldInfo &field, int algo,
                        bool hintScale, bool hintPosition,
                        int timeoutSecs, const QString &label);

    private Q_SLOTS:
        void initTestCase();
        void benchmarkMultiAlgo_data();
        void benchmarkMultiAlgo();
};

void TestSolverBenchmark::initTestCase()
{
    m_fitsDir = qEnvironmentVariable("FITS_DIR",
                                     QDir::homePath() + "/Pictures");
    m_indexDir = qEnvironmentVariable("INDEX_DIR",
                                      QDir::homePath() + "/Library/Application Support/kstars/astrometry");

    if (!QDir(m_fitsDir).exists())
        QSKIP("FITS directory not found. Set FITS_DIR env var.");
    if (!QDir(m_indexDir).exists())
        QSKIP("Index file directory not found. Set INDEX_DIR env var.");

    qInfo() << "FITS dir :" << m_fitsDir;
    qInfo() << "Index dir:" << m_indexDir;
}

bool TestSolverBenchmark::solveField(const FieldInfo &field, int algo,
                                     bool hintScale, bool hintPosition,
                                     int timeoutSecs, const QString &label)
{
    QString path = m_fitsDir + "/" + field.filename;

    fileio imageLoader;
    imageLoader.logToSignal = false;
    if (!imageLoader.loadImage(path))
    {
        qWarning() << "Failed to load" << path;
        return false;
    }

    FITSImage::Statistic stats = imageLoader.getStats();
    uint8_t *imageBuffer = imageLoader.getImageBuffer();

    StellarSolver solver(stats, imageBuffer);
    solver.setIndexFolderPaths(QStringList() << m_indexDir);
    solver.setParameterProfile(SSolver::Parameters::PARALLEL_SMALLSCALE);

    SSolver::Parameters params = solver.getCurrentParameters();
    params.multiAlgorithm = static_cast<SSolver::MultiAlgo>(algo);
    params.solverTimeLimit = timeoutSecs;
    solver.setParameters(params);

    if (hintScale)
        solver.setSearchScale(field.pixscale * 0.9, field.pixscale * 1.1,
                              SSolver::ARCSEC_PER_PIX);
    if (hintPosition)
        solver.setSearchPositionInDegrees(field.ra, field.dec);

    QElapsedTimer timer;
    timer.start();
    bool success = solver.solve();
    double elapsed = timer.elapsed() / 1000.0;

    if (!success)
    {
        qInfo() << qPrintable(QString("  %1  TIMEOUT (%2s)")
                                  .arg(label, -14)
                                  .arg(timeoutSecs));
        return false;
    }

    FITSImage::Solution solution = solver.getSolution();
    double raDiff = std::abs(solution.ra - field.ra);
    if (raDiff > 180.0) raDiff = 360.0 - raDiff;
    double decDiff = std::abs(solution.dec - field.dec);
    double errDeg = std::sqrt(raDiff * raDiff + decDiff * decDiff);

    qInfo() << qPrintable(QString("  %1  %2s  scale %3  error %4 deg")
                              .arg(label, -14)
                              .arg(elapsed, 5, 'f', 2)
                              .arg(solution.pixscale, 0, 'f', 3)
                              .arg(errDeg, 0, 'f', 4));

    return (raDiff < 0.5 && decDiff < 0.5);
}

void TestSolverBenchmark::benchmarkMultiAlgo_data()
{
    QTest::addColumn<QString>("filename");
    QTest::addColumn<QString>("label");
    QTest::addColumn<double>("ra");
    QTest::addColumn<double>("dec");
    QTest::addColumn<double>("pixscale");

    // Fields from KStars solver benchmark, rendered at 1280x1024, 2.06 arcsec/px
    QTest::newRow("pleiades")      << "pleiades.fits"      << "M45 Pleiades"           << 56.9  << 24.1  << 2.06;
    QTest::newRow("ngc4535")       << "ngc4535.fits"        << "NGC 4535"               << 188.6 << 8.2   << 2.06;
    QTest::newRow("galactic-pole") << "galactic-pole.fits"  << "high galactic latitude" << 192.9 << 27.1  << 2.06;
    QTest::newRow("orion")         << "orion.fits"          << "Orion galactic plane"   << 83.8  << -5.4  << 2.06;
    QTest::newRow("m5")            << "m5.fits"             << "M5 globular cluster"    << 229.6 << 2.1   << 2.06;
    QTest::newRow("m74")           << "m74.fits"            << "M74 spiral galaxy"      << 24.2  << 15.8  << 2.06;
}

void TestSolverBenchmark::benchmarkMultiAlgo()
{
    QFETCH(QString, filename);
    QFETCH(QString, label);
    QFETCH(double, ra);
    QFETCH(double, dec);
    QFETCH(double, pixscale);

    FieldInfo field { filename, label, ra, dec, pixscale };

    QString path = m_fitsDir + "/" + filename;
    if (!QFile::exists(path))
        QSKIP(qPrintable(QString("FITS file not found: %1").arg(path)));

    qInfo() << qPrintable(label);

    struct { int algo; QString name; } algorithms[] = {
        { MULTI_SCALES, "MULTI_SCALES" },
        { MULTI_DEPTHS, "MULTI_DEPTHS" },
    };

    struct { bool scale; bool position; int timeout; QString name; } hints[] = {
        { true,  true,  20, "scale+pos" },
        { true,  false, 30, "scale"     },
        { false, true,  30, "pos"       },
    };

    for (const auto &h : hints)
    {
        qInfo() << qPrintable(QString("  -- %1 --").arg(h.name));
        for (const auto &a : algorithms)
        {
            QVERIFY2(solveField(field, a.algo,
                                h.scale, h.position, h.timeout, a.name),
                     qPrintable(QString("%1 / %2 / %3 failed")
                                    .arg(label, a.name, h.name)));
        }
    }
}

QTEST_GUILESS_MAIN(TestSolverBenchmark)

#include "testsolverbenchmark.moc"
