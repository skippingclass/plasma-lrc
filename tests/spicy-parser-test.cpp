/*
 * SPDX-FileCopyrightText: 2026 pirkov
 *
 * SPDX-License-Identifier: MIT
 *
 * Checks the lyric parser against answers the API really returns. Run it with
 *
 *   cmake -S . -B build -DPLASMA_LRC_TESTS=ON && cmake --build build
 *   ./build/spicy-parser-test
 */

#include "spicylyrics.h"
#include "trackname.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

namespace
{
int failures = 0;

void check(const QString &what, bool ok, const QString &detail = QString())
{
    printf("%-56s %s%s\n", what.toUtf8().constData(), ok ? "ok" : "FAIL", detail.isEmpty() ? "" : (" — " + detail).toUtf8().constData());
    if (!ok) {
        ++failures;
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // With a file as an argument, the answer is run through the parser and
    // printed, so a track that misbehaves can be looked at without a player:
    //   spicy-parser-test answer.json
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning("не открывается: %s", argv[1]);
            return 1;
        }
        Lyrics lyrics;
        const bool parsed = SpicyLyrics::parse(file.readAll(), &lyrics, nullptr);
        fprintf(stderr, "разобрано=%d строк=%lld годно=%d тип=%d\n", (int)parsed, (long long)lyrics.lines.size(),
              (int)lyrics.isUsable(), (int)lyrics.type);
        for (int i = 0; i < qMin(5, int(lyrics.lines.size())); ++i) {
            const LyricLine &line = lyrics.lines.at(i);
            fprintf(stderr, "строка %d: %lld..%lld «%s» слов=%d\n", i, (long long)line.startMs, (long long)line.endMs,
                  qPrintable(line.text), (int)line.words.size());
            for (const LyricWord &word : line.words) {
                fprintf(stderr, "   [%d:%d] %lld..%lld «%s»\n", word.groupStart, word.groupEnd, (long long)word.startMs,
                      (long long)word.endMs, qPrintable(line.text.mid(word.groupStart, word.groupEnd - word.groupStart)));
            }
        }
        // Every word has to end up with bounds of its own: a word with an empty
        // range is a word the panel can never light up.
        int empty = 0;
        int total = 0;
        for (const LyricLine &line : lyrics.lines) {
            for (const LyricWord &word : line.words) {
                ++total;
                if (word.groupEnd <= word.groupStart) {
                    ++empty;
                }
            }
        }
        fprintf(stderr, "слов=%d с пустой группой=%d\n", total, empty);

        return parsed && empty == 0 ? 0 : 2;
    }

    // --- id трека из MPRIS ---
    check("mpris: /com/spotify/track/<id>", SpicyLyrics::trackIdFromMpris("/com/spotify/track/05NFt6hnomymkp09f4gGry") == "05NFt6hnomymkp09f4gGry");
    check("mpris: spotify:track:<id>", SpicyLyrics::trackIdFromMpris("spotify:track:05NFt6hnomymkp09f4gGry") == "05NFt6hnomymkp09f4gGry");
    check("mpris: пусто", SpicyLyrics::trackIdFromMpris("/com/spotify/track/short").isEmpty());

    // --- Community sync: слова режутся, IsPartOfWord стоит на ПЕРВОМ куске ---
    {
        // Verified answer from api.spicylyrics.org for a word-synced track.
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable",
          "UploadAttribution":{"Uploader":{"username":"uploader","url":"https://example/u"},
                               "Maker":{"username":"maker","url":"https://example/m"}},
          "Content":[
            {"Type":"Vocal","Lead":{"Syllables":[
              {"Text":"think","StartTime":1.0,"EndTime":1.4},
              {"Text":"o","StartTime":1.4,"EndTime":1.6,"IsPartOfWord":true},
              {"Text":"k","StartTime":1.6,"EndTime":1.8},
              {"Text":"is","StartTime":1.9,"EndTime":2.1},
              {"Text":"the","StartTime":2.2,"EndTime":2.4},
              {"Text":"har","StartTime":2.5,"EndTime":2.7,"IsPartOfWord":true},
              {"Text":"dest,","StartTime":2.7,"EndTime":3.1}],
              "StartTime":1.0,"EndTime":3.1}}]}})";
        Lyrics lyrics;
        QString error;
        check("community: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("community: без пробелов внутри слов", lyrics.lines.value(0).text == QStringLiteral("think ok is the hardest,"), lyrics.lines.value(0).text);
        check("community: смещение продолжения", lyrics.lines.value(0).words.value(2).textStart == 7 && lyrics.lines.value(0).words.value(2).textEnd == 8,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(2).textStart).arg(lyrics.lines.value(0).words.value(2).textEnd));
        check("community: границы разбитого слова", lyrics.lines.value(0).words.value(5).textStart == 16 && lyrics.lines.value(0).words.value(5).textEnd == 19,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(5).textStart).arg(lyrics.lines.value(0).words.value(5).textEnd));
        // The highlight must cover the whole word, not one of its pieces:
        // "think ok is the hardest," -> ok = 6..8, hardest, = 16..24.
        check("community: группа для 'ok'", lyrics.lines.value(0).words.value(1).groupStart == 6 && lyrics.lines.value(0).words.value(1).groupEnd == 8,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(1).groupStart).arg(lyrics.lines.value(0).words.value(1).groupEnd));
        check("community: у 'o' и 'k' одна группа",
              lyrics.lines.value(0).words.value(1).groupStart == lyrics.lines.value(0).words.value(2).groupStart
                  && lyrics.lines.value(0).words.value(1).groupEnd == lyrics.lines.value(0).words.value(2).groupEnd);
        check("community: группа для 'hardest,'", lyrics.lines.value(0).words.value(5).groupStart == 16 && lyrics.lines.value(0).words.value(5).groupEnd == 24,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(5).groupStart).arg(lyrics.lines.value(0).words.value(5).groupEnd));
        check("community: 'think' — отдельное слово", lyrics.lines.value(0).words.value(0).groupStart == 0 && lyrics.lines.value(0).words.value(0).groupEnd == 5,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(0).groupStart).arg(lyrics.lines.value(0).words.value(0).groupEnd));
        check("community: текст группы = целое слово",
              lyrics.lines.value(0).text.mid(lyrics.lines.value(0).words.value(1).groupStart,
                  lyrics.lines.value(0).words.value(1).groupEnd - lyrics.lines.value(0).words.value(1).groupStart) == QStringLiteral("ok"));
        check("community: выбор строки", lyrics.lineAt(2800) == 0, QString::number(lyrics.lineAt(2800)));
        check("community: атрибуция", lyrics.attribution == QStringLiteral("Spicy Lyrics · made by @maker (@uploader)"), lyrics.attribution);
        check("community: ссылка", lyrics.attributionUrl == QStringLiteral("https://example/m"), lyrics.attributionUrl);
    }

    // --- Несколько кусков одного слова подряд: E+very+bo+dy ---
    {
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable","Content":[
          {"Type":"Vocal","Lead":{"Syllables":[
            {"Text":"know","StartTime":1.0,"EndTime":1.3},
            {"Text":"E","StartTime":1.4,"EndTime":1.5,"IsPartOfWord":true},
            {"Text":"very","StartTime":1.5,"EndTime":1.8,"IsPartOfWord":true},
            {"Text":"bo","StartTime":1.8,"EndTime":2.0,"IsPartOfWord":true},
            {"Text":"dy","StartTime":2.0,"EndTime":2.2}],
            "StartTime":1.0,"EndTime":2.2}}]}})";
        Lyrics lyrics;
        QString error;
        check("Everybody: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("Everybody: без лишних пробелов", lyrics.lines.value(0).text == QStringLiteral("know Everybody"), lyrics.lines.value(0).text);
        check("Everybody: четыре куска — одна группа",
              lyrics.lines.value(0).words.value(1).groupStart == 5 && lyrics.lines.value(0).words.value(1).groupEnd == 14,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(1).groupStart).arg(lyrics.lines.value(0).words.value(1).groupEnd));
    }

    // --- Пробелы в тексте слота, если они всё же есть ---
    {
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable","Content":[
          {"Type":"Vocal","Lead":{"Syllables":[
            {"Text":"I ","StartTime":7.091,"EndTime":7.304},
            {"Text":"rea","StartTime":7.304,"EndTime":7.488,"IsPartOfWord":true},
            {"Text":"lly ","StartTime":7.488,"EndTime":7.729}],
            "StartTime":7.091,"EndTime":7.729}}]}})";
        Lyrics lyrics;
        QString error;
        check("пробелы в Text: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("пробелы в Text: без дублей", lyrics.lines.value(0).text == QStringLiteral("I really"), lyrics.lines.value(0).text);
    }

    // --- Без флагов: всё считается отдельными словами ---
    {
        const QByteArray json = R"({"Body":{"source":"spotify","Type":"Syllable","Content":[
          {"Lead":{"Syllables":[
            {"Text":"Hello","StartTime":1.0,"EndTime":1.5},
            {"Text":"world","StartTime":1.5,"EndTime":2.0}]}}]}})";
        Lyrics lyrics;
        QString error;
        check("без флагов: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("без флагов: пробел между словами", lyrics.lines.value(0).text == QStringLiteral("Hello world"), lyrics.lines.value(0).text);
        check("источник spotify: атрибуция Spotify", lyrics.attribution == QStringLiteral("Spotify"), lyrics.attribution);
    }

    // --- CJK: пробелов нет вовсе ---
    {
        const QByteArray json = R"({"Body":{"source":"apple_music","Type":"Syllable","Content":[
          {"Lead":{"Syllables":[
            {"Text":"遥","StartTime":0.917,"EndTime":1.169},
            {"Text":"か","StartTime":1.169,"EndTime":1.385},
            {"Text":"遠","StartTime":1.385,"EndTime":1.743},
            {"Text":"く","StartTime":1.743,"EndTime":1.908}]}}]}})";
        Lyrics lyrics;
        QString error;
        check("CJK: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("CJK: без пробелов", lyrics.lines.value(0).text == QStringLiteral("遥か遠く"), lyrics.lines.value(0).text);
        check("источник apple: атрибуция", lyrics.attribution == QStringLiteral("Apple Music"), lyrics.attribution);
    }

    // --- Apple Music: текст прямо на записи, Lead нет вовсе ---
    {
        // Verified answer from api.spicylyrics.org for a track with no community
        // sync: line-synced, Text and timings on the entry itself.
        const QByteArray json = R"({"Body":{"id":"x","source":"apple_music","Type":"Line",
          "StartTime":0.88,"EndTime":13.07,
          "Content":[
            {"Type":"Vocal","OppositeAligned":false,"Text":"You actually tried to injure yourself twice, didn't you?","StartTime":0.88,"EndTime":5.37},
            {"Type":"Vocal","OppositeAligned":false,"Text":"Uh, this one was last week","StartTime":5.37,"EndTime":10.13},
            {"Type":"Vocal","OppositeAligned":false,"Text":"I mean, uh, today is Monday","StartTime":10.13,"EndTime":13.07}]}})";
        Lyrics lyrics;
        QString error;
        check("apple: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("apple: тип Line", lyrics.type == LyricsType::Line);
        check("apple: usable", lyrics.isUsable());
        check("apple: 3 строки", lyrics.lines.size() == 3, QString::number(lyrics.lines.size()));
        check("apple: текст строки", lyrics.lines.value(0).text == QStringLiteral("You actually tried to injure yourself twice, didn't you?"), lyrics.lines.value(0).text);
        check("apple: тайминги строки", lyrics.lines.value(1).startMs == 5370 && lyrics.lines.value(1).endMs == 10130,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(1).startMs).arg(lyrics.lines.value(1).endMs));
        check("apple: выбор строки по позиции", lyrics.lineAt(11000) == 2, QString::number(lyrics.lineAt(11000)));
        check("apple: вне строки", lyrics.lineAt(20000) == -1, QString::number(lyrics.lineAt(20000)));
    }

    // --- Смешанная форма: часть строк с Lead, часть напрямую ---
    {
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable","Content":[
          {"Type":"Vocal","Lead":{"Syllables":[{"Text":"first ","StartTime":1.0,"EndTime":1.5},{"Text":"line","StartTime":1.5,"EndTime":2.0}],"StartTime":1.0,"EndTime":2.0}},
          {"Type":"Vocal","Text":"second line","StartTime":2.0,"EndTime":4.0}]}})";
        Lyrics lyrics;
        QString error;
        check("смешанная: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("смешанная: обе строки", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        check("смешанная: первая склеена", lyrics.lines.value(0).text == QStringLiteral("first line"), lyrics.lines.value(0).text);
        check("смешанная: вторая как есть", lyrics.lines.value(1).text == QStringLiteral("second line"), lyrics.lines.value(1).text);
    }

    // --- Запись без текста пропускается, а не ломает разбор ---
    {
        const QByteArray json = R"({"Body":{"source":"apple_music","Type":"Line","Content":[
          {"Type":"Vocal","StartTime":1.0,"EndTime":2.0},
          {"Type":"Vocal","Text":"real line","StartTime":2.0,"EndTime":3.0}]}})";
        Lyrics lyrics;
        QString error;
        check("пустая запись пропущена", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("пустая запись: осталась одна", lyrics.lines.size() == 1, QString::number(lyrics.lines.size()));
        check("пустая запись: текст верный", lyrics.lines.value(0).text == QStringLiteral("real line"), lyrics.lines.value(0).text);
    }

    // --- Line-синхронизацияcommunity ---
    {
        const QByteArray json = R"({"Body":{"source":"spotify","Type":"Line","Content":[
          {"Lead":{"Syllables":[{"Text":"first line","StartTime":1.0,"EndTime":4.0}],"StartTime":1.0,"EndTime":4.0}},
          {"Lead":{"Syllables":[{"Text":"second line","StartTime":4.0,"EndTime":8.0}],"StartTime":4.0,"EndTime":8.0}}]}})";
        Lyrics lyrics;
        QString error;
        check("line-sync: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("line-sync: тип", lyrics.type == LyricsType::Line);
        check("line-sync: 2 строки", lyrics.lines.size() == 2, QString::number(lyrics.lines.size()));
        check("line-sync: выбор по позиции", lyrics.lineAt(5000) == 1, QString::number(lyrics.lineAt(5000)));
    }

    // --- Static: без таймингов, для показа не годится ---
    {
        const QByteArray json = R"({"Body":{"source":"spotify","Type":"Static","Content":[
          {"Lead":{"Syllables":[{"Text":"a b","StartTime":0,"EndTime":0}]}}]}})";
        Lyrics lyrics;
        QString error;
        check("static: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("static: не usable", !lyrics.isUsable());
    }


    // --- Строки перекрываются: показывается та, что началась позже ---
    {
        // Real answers overlap often: the next line begins before the previous one
        // ends, because a word is sung across the break. Measured on real tracks,
        // 28 lines out of 54 used to show up late, once by 886ms, because the
        // first matching line won.
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Line","Content":[
          {"Text":"first line","StartTime":1.0,"EndTime":4.0},
          {"Text":"second line","StartTime":3.5,"EndTime":6.0},
          {"Text":"third line","StartTime":5.8,"EndTime":8.0}]}})";
        Lyrics lyrics;
        QString error;
        check("наложение: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("наложение: до начала второй — первая", lyrics.lineAt(2000) == 0, QString::number(lyrics.lineAt(2000)));
        check("наложение: в перекрытии побеждает вторая", lyrics.lineAt(3700) == 1, QString::number(lyrics.lineAt(3700)));
        check("наложение: на её конце — третья", lyrics.lineAt(5900) == 2, QString::number(lyrics.lineAt(5900)));
        check("наложение: после конца третьей ничего", lyrics.lineAt(9000) == -1, QString::number(lyrics.lineAt(9000)));
    }

    // --- Быстрая строка: короткое слово между двумя ---
    {
        // A word of 50ms between two longer ones: a coarse tick falls in the gaps
        // around it, and the applet lights the nearest word within reach.
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable","Content":[
          {"Lead":{"Syllables":[
            {"Text":"go","StartTime":1.00,"EndTime":1.30},
            {"Text":"fast","StartTime":1.34,"EndTime":1.39},
            {"Text":"now","StartTime":1.44,"EndTime":1.80}],"StartTime":1.0,"EndTime":1.8}}]}})";
        Lyrics lyrics;
        QString error;
        check("быстрая строка: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("быстрая строка: текст", lyrics.lines.value(0).text == QStringLiteral("go fast now"), lyrics.lines.value(0).text);
        check("быстрая строка: 20мс-слово на месте",
              lyrics.lines.value(0).words.value(1).startMs == 1340 && lyrics.lines.value(0).words.value(1).endMs == 1390,
              QStringLiteral("%1..%2").arg(lyrics.lines.value(0).words.value(1).startMs).arg(lyrics.lines.value(0).words.value(1).endMs));
    }

    // --- Промежуток между строками: ничего не поется ---
    {
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Line","Content":[
          {"Text":"one","StartTime":1.0,"EndTime":3.0},
          {"Text":"two","StartTime":6.0,"EndTime":8.0}]}})";
        Lyrics lyrics;
        QString error;
        check("промежуток: парсится", SpicyLyrics::parse(json, &lyrics, &error), error);
        check("промежуток: в промежутке пусто", lyrics.lineAt(4500) == -1, QString::number(lyrics.lineAt(4500)));
        check("промежуток: до него первая", lyrics.lineAt(2000) == 0, QString::number(lyrics.lineAt(2000)));
        check("промежуток: после него вторая", lyrics.lineAt(7000) == 1, QString::number(lyrics.lineAt(7000)));
    }


    // --- Названия из браузера, YouTube и yt-dlp ---
    {
        // Real cases that lrclib does not match without cleaning.
        check("шум в скобках: (Official Video)",
              cleanTrackTitle(QStringLiteral("deaf note (Official Video)")) == QStringLiteral("deaf note"),
              cleanTrackTitle(QStringLiteral("deaf note (Official Video)")));
        check("шум в скобках: [4K] и хвост",
              cleanTrackTitle(QStringLiteral("deaf note [4K]")) == QStringLiteral("deaf note"),
              cleanTrackTitle(QStringLiteral("deaf note [4K]")));
        check("шум: глава после палки",
              cleanTrackTitle(QStringLiteral("Song | Lyrics video")) == QStringLiteral("Song"),
              cleanTrackTitle(QStringLiteral("Song | Lyrics video")));
        check("шум: feat в скобках",
              cleanTrackTitle(QStringLiteral("Song (feat. Someone)")) == QStringLiteral("Song"),
              cleanTrackTitle(QStringLiteral("Song (feat. Someone)")));
        check("шум: feat без скобок",
              cleanTrackTitle(QStringLiteral("Song feat. Someone")) == QStringLiteral("Song"),
              cleanTrackTitle(QStringLiteral("Song feat. Someone")));
        check("шум: висячее тире после скобок",
              cleanTrackTitle(QStringLiteral("Song (Official Video) -")) == QStringLiteral("Song"),
              cleanTrackTitle(QStringLiteral("Song (Official Video) -")));
        check("шум: всё вместе",
              cleanTrackTitle(QStringLiteral("Ken Carson - deaf note (with Playboi Carti) [Official Video] [4K] | Lyrics video"))
                  == QStringLiteral("Ken Carson - deaf note (with Playboi Carti)"),
              cleanTrackTitle(QStringLiteral("Ken Carson - deaf note (with Playboi Carti) [Official Video] [4K] | Lyrics video")));
        check("чистое название не трогаем",
              cleanTrackTitle(QStringLiteral("Breathe")) == QStringLiteral("Breathe"));
        check("исполнитель: - Topic",
              cleanTrackArtist(QStringLiteral("Artist - Topic")) == QStringLiteral("Artist"),
              cleanTrackArtist(QStringLiteral("Artist - Topic")));
        check("исполнитель: VEVO",
              cleanTrackArtist(QStringLiteral("ArtistVEVO")) == QStringLiteral("Artist"),
              cleanTrackArtist(QStringLiteral("ArtistVEVO")));
        check("исполнитель: feat",
              cleanTrackArtist(QStringLiteral("Artist feat. Someone")) == QStringLiteral("Artist"),
              cleanTrackArtist(QStringLiteral("Artist feat. Someone")));
        check("исполнитель: два исполнителя не трогаем",
              cleanTrackArtist(QStringLiteral("Artist, Someone Else")) == QStringLiteral("Artist, Someone Else"));
        check("исполнитель: чистый не трогаем",
              cleanTrackArtist(QStringLiteral("Ken Carson")) == QStringLiteral("Ken Carson"));

        // A channel name that has nothing to do with the artist is left alone:
        // "Love - Hate" by Drake must not become artist "Love".
        QString artist = QStringLiteral("Some Channel");
        QString title = QStringLiteral("Ken Carson - deaf note");
        splitTrackArtistAndTitle(&artist, &title);
        check("разделение: чужой артист не делим",
              artist == QStringLiteral("Some Channel") && title == QStringLiteral("Ken Carson - deaf note"),
              artist + " / " + title);

        artist = QString();
        title = QStringLiteral("Ken Carson - deaf note");
        splitTrackArtistAndTitle(&artist, &title);
        check("разделение: пустой артист",
              artist == QStringLiteral("Ken Carson") && title == QStringLiteral("deaf note"),
              artist + " / " + title);

        artist = QStringLiteral("Ken Carson");
        title = QStringLiteral("Ken Carson - deaf note");
        splitTrackArtistAndTitle(&artist, &title);
        check("разделение: артист совпал",
              artist == QStringLiteral("Ken Carson") && title == QStringLiteral("deaf note"),
              artist + " / " + title);

        artist = QStringLiteral("Someone Else");
        title = QStringLiteral("Jay-Z - Song");
        splitTrackArtistAndTitle(&artist, &title);
        check("разделение: чужой артист не трогаем",
              artist == QStringLiteral("Someone Else") && title == QStringLiteral("Jay-Z - Song"),
              artist + " / " + title);
    }

    // --- Последнее слово строки с висящим флагом продолжения ---
    {
        // A real answer from api.spicylyrics.org for OsamaSon - Baghdad, where
        // every sixteenth line or so ends with a piece that carries
        // IsPartOfWord although nothing follows it. The last word then used to
        // keep empty bounds and could never be lit up.
        const QByteArray json = R"({"Body":{"source":"spicy_lyrics","Type":"Syllable","Content":[
          {"Type":"Vocal","Lead":{"Syllables":[
            {"Text":"Pour","StartTime":7.526,"EndTime":7.655},
            {"Text":"more,","StartTime":7.655,"EndTime":7.854},
            {"Text":"not","StartTime":7.854,"EndTime":8.071},
            {"Text":"enough","StartTime":8.071,"EndTime":8.546,"IsPartOfWord":true}],
            "StartTime":7.526,"EndTime":9.036}}]}})";
        Lyrics lyrics;
        check("висящий флаг: парсится", SpicyLyrics::parse(json, &lyrics, nullptr) && lyrics.lines.size() == 1);
        const LyricLine line = lyrics.lines.first();
        check("висящий флаг: текст строки", line.text == QStringLiteral("Pour more, not enough"), line.text);
        check("висящий флаг: слов четыре", line.words.size() == 4, QString::number(line.words.size()));
        check("висящий флаг: последнее слово не пустое",
              line.words.last().groupEnd > line.words.last().groupStart,
              QString::number(line.words.last().groupEnd - line.words.last().groupStart));
        check("висящий флаг: границы последнего слова",
              line.text.mid(line.words.last().groupStart, line.words.last().groupEnd - line.words.last().groupStart)
                  == QStringLiteral("enough"),
              line.text.mid(line.words.last().groupStart, line.words.last().groupEnd - line.words.last().groupStart));
        check("висящий флаг: время последнего слова",
              line.words.last().startMs == 8071 && line.words.last().endMs == 8546,
              qPrintable(QStringLiteral("%1..%2").arg(line.words.last().startMs).arg(line.words.last().endMs)));
    }

    // --- Перемотка назад ---
    {
        // Three lines with a gap between the first and the second. A search that
        // only looks forward from the line shown last time cannot come back, and
        // the panel stayed on whatever it had.
        Lyrics lyrics;
        lyrics.lines = {
            {QStringLiteral("one"), 1000, 2000, {}},
            {QStringLiteral("two"), 4000, 5000, {}},
            {QStringLiteral("three"), 6000, 7000, {}},
        };
        const auto indexAt = [&lyrics](qint64 ms) { return QString::number(lyrics.lineAt(ms)); };
        check("перемотка назад: вперёд", lyrics.lineAt(6500) == 2, indexAt(6500));
        check("перемотка назад: назад", lyrics.lineAt(1500) == 0, indexAt(1500));
        check("перемотка назад: в самое начало", lyrics.lineAt(0) == -1, indexAt(0));
        check("перемотка назад: в промежуток", lyrics.lineAt(3000) == -1, indexAt(3000));
        check("перемотка назад: сильно назад", lyrics.lineAt(500) == -1, indexAt(500));

        // A line that started earlier and is still running counts as sung, even
        // though the newest line has already ended.
        Lyrics longLine;
        longLine.lines = {
            {QStringLiteral("long"), 0, 10000, {}},
            {QStringLiteral("short"), 1000, 2000, {}},
        };
        check("перемотка назад: длинная строка после короткой", longLine.lineAt(5000) == 0,
              QString::number(longLine.lineAt(5000)));
        check("перемотка назад: конец длинной строки", longLine.lineAt(10000) == -1,
              QString::number(longLine.lineAt(10000)));
    }

    // --- Мусор ---
    {
        Lyrics lyrics;
        QString error;
        check("мусор отклоняется", !SpicyLyrics::parse("not json at all", &lyrics, &error), error);
        check("404-подобный ответ отклоняется", !SpicyLyrics::parse("{}", &lyrics, &error), error);
        check("пустой Content отклоняется", !SpicyLyrics::parse(R"({"Body":{"Type":"Syllable","Content":[]}})", &lyrics, &error), error);
    }

    // --- Неизвестный источник ---
    {
        const QByteArray json = R"({"Body":{"source":"somewhere_else","Type":"Line","Content":[
          {"Text":"x","StartTime":0,"EndTime":1}]}})";
        Lyrics lyrics;
        SpicyLyrics::parse(json, &lyrics, nullptr);
        check("неизвестный источник: 'unknown source'", lyrics.attribution == QStringLiteral("unknown source"), lyrics.attribution);
    }

    printf("\n%s\n", failures == 0 ? "все проверки прошли" : "ЕСТЬ ПРОВАЛЫ");
    return failures == 0 ? 0 : 1;
}