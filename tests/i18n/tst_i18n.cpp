// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <QtTest>

#include "concurrency.h"
#include "ts/base/buildinfo.h"
#include "ts/i18n/pseudotranslator.h"
#include "ts/i18n/translationloader.h"

/** Unit tests for ts/i18n: translation loading and the pseudo-locale. */
class TestI18n : public QObject
{
  Q_OBJECT

private slots:
  void englishIsEmbedded()
  {
    QTranslator translator;
    QCOMPARE(ts::loadTranslation(translator, QLocale(QLocale::English)),
             QStringLiteral("en"));
    QVERIFY(!translator.isEmpty());
  }

  void unknownLocaleFallsBackToEnglish_data()
  {
    QTest::addColumn<QString>("locale");
    QTest::newRow("de_DE") << QStringLiteral("de_DE");
    QTest::newRow("ja_JP") << QStringLiteral("ja_JP");
    QTest::newRow("en_GB") << QStringLiteral("en_GB");
    QTest::newRow("en_US") << QStringLiteral("en_US");
    QTest::newRow("C") << QStringLiteral("C");
  }

  void unknownLocaleFallsBackToEnglish()
  {
    QFETCH(QString, locale);
    QTranslator translator;
    QCOMPARE(ts::loadTranslation(translator, QLocale(locale)),
             QStringLiteral("en"));
  }

  void missingDirectoryLoadsNothing()
  {
    QTranslator translator;
    QVERIFY(ts::loadTranslation(translator, QLocale::system(),
                                QStringLiteral(":/no-such-dir"))
                .isEmpty());
  }

  void englishTranslatesProgramName()
  {
    QTranslator translator;
    ts::loadTranslation(translator, QLocale(QLocale::English));
    QCoreApplication::installTranslator(&translator);
    QCOMPARE(ts::programName(), QStringLiteral("Terra Soar"));
    QCoreApplication::removeTranslator(&translator);
  }

  void pseudoTranslate_data()
  {
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("expected");
    QTest::newRow("word") << QStringLiteral("Quit")
                          << QStringLiteral("[Ǫûîţ~~]");
    QTest::newRow("mnemonic")
        << QStringLiteral("&File") << QStringLiteral("[&Ƒîĺé~~]");
    QTest::newRow("literal-ampersand")
        << QStringLiteral("A && B") << QStringLiteral("[Å && Ɓ~~]");
    QTest::newRow("placeholders")
        << QStringLiteral("%1 of %L2") << QStringLiteral("[%1 öƒ %L2~~~]");
    QTest::newRow("plural")
        << QStringLiteral("%n tile(s)") << QStringLiteral("[%n ţîĺé(š)~~~]");
    QTest::newRow("html") << QStringLiteral("<b>Hi</b>")
                          << QStringLiteral("[<b>Ĥî</b>~]");
    QTest::newRow("percent-sign")
        << QStringLiteral("50%") << QStringLiteral("[50%~]");
    QTest::newRow("empty") << QString() << QStringLiteral("[]");
  }

  void pseudoTranslate()
  {
    QFETCH(QString, source);
    QFETCH(QString, expected);
    QCOMPARE(ts::PseudoTranslator::pseudoTranslate(source), expected);
  }

  void pseudoIsAtLeastThirtyPercentLonger()
  {
    const QString source = QStringLiteral("Show the program version");
    const QString pseudo = ts::PseudoTranslator::pseudoTranslate(source);
    QVERIFY(static_cast<double>(pseudo.size()) >=
            static_cast<double>(source.size()) * 1.3);
  }

  void accentTablesCoverAlphabet()
  {
    const QString upper = ts::PseudoTranslator::pseudoTranslate(
        QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
    const QString lower = ts::PseudoTranslator::pseudoTranslate(
        QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
    for (const QString& text : {upper, lower}) {
      for (const QChar c : text) {
        QVERIFY2(!(c >= QLatin1Char('A') && c <= QLatin1Char('Z')) &&
                     !(c >= QLatin1Char('a') && c <= QLatin1Char('z')),
                 qPrintable(text));
      }
    }
  }

  void pseudoPlaceholdersStillFormat()
  {
    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    const QString text =
        QCoreApplication::translate("ctx", "%1 of %2").arg(3).arg(7);
    const QString plural =
        QCoreApplication::translate("ctx", "%n tile(s)", nullptr, 5);
    QCoreApplication::removeTranslator(&translator);
    QCOMPARE(text, QStringLiteral("[3 öƒ 7~~~]"));
    QVERIFY(plural.startsWith(QLatin1String("[5 ")));
  }

  void isPseudoTranslated()
  {
    QVERIFY(ts::PseudoTranslator::isPseudoTranslated(
        ts::PseudoTranslator::pseudoTranslate(QStringLiteral("&Help"))));
    QVERIFY(!ts::PseudoTranslator::isPseudoTranslated(QStringLiteral("Help")));
    QVERIFY(!ts::PseudoTranslator::isPseudoTranslated(QStringLiteral("[Hel")));
  }

  void concurrentSharedPseudoTranslator()
  {
    const ts::PseudoTranslator translator;
    const QString expected =
        ts::PseudoTranslator::pseudoTranslate(QStringLiteral("About %1"));
    const int failures =
        ts::test::runConcurrently(ts::test::concurrentThreadCount, [&](int) {
          for (int i = 0; i < 2000; ++i) {
            if (translator.translate("ctx", "About %1") != expected) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void concurrentOwnLoaders()
  {
    const int failures =
        ts::test::runConcurrently(ts::test::concurrentThreadCount, [](int) {
          for (int i = 0; i < 50; ++i) {
            QTranslator translator;
            if (ts::loadTranslation(translator, QLocale(QLocale::German)) !=
                QLatin1String("en")) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkPseudoTranslate()
  {
    const QString source =
        QStringLiteral("Show the program version and licence");
    QString out;
    QBENCHMARK
    {
      out = ts::PseudoTranslator::pseudoTranslate(source);
    }
    QVERIFY(!out.isEmpty());
  }
};

QTEST_GUILESS_MAIN(TestI18n)
#include "tst_i18n.moc"
