/*
    Copyright (C) 2017 Robin Burchell <robin+git@viroteck.net>
    Copyright 2011-2012 Heikki Holstila <heikki.holstila@gmail.com>

    This work is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This work is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this work.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QGuiApplication>
#include <QClipboard>
#include <QDebug>
#include <QRegularExpression>

#include "terminal.h"
#include "ptyiface.h"
#include "textrender.h"

#if defined(Q_OS_MAC)
# define MyControlModifier Qt::MetaModifier
#else
# define MyControlModifier Qt::ControlModifier
#endif

/*!
    \class TerminalLine
    \internal

    A TerminalLine represents a single line of contents in the terminal's buffer.
*/

/*!
    \class TerminalBuffer
    \internal

    A TerminalBuffer is a collection of TerminalLine instances.
*/

static bool charIsHexDigit(QChar ch)
{
    if (ch.isDigit()) // 0-9
        return true;
    else if (ch.toLatin1() >= 65 && ch.toLatin1() <= 70) // A-F
        return true;
    else if (ch.toLatin1() >= 97 && ch.toLatin1() <= 102) // a-f
        return true;

    return false;
}

QRgb Terminal::defaultFgColor;
QRgb Terminal::defaultBgColor;

Terminal::Terminal(QObject *parent)
    : QObject(parent)
    , iTermSize(0,0)
    , iEmitCursorChangeSignal(true)
    , iShowCursor(true)
    , iUseAltScreenBuffer(false)
    , iAppCursorKeys(false)
    , iReplaceMode(false)
    , iNewLineMode(false)
    , iBackBufferScrollPos(0)
    , m_dispatch_timer(0)
{
    iColorTable.reserve(256);

    //normal
    iColorTable.append(QColor(0, 0, 0).rgb());
    iColorTable.append(QColor(210, 0, 0).rgb());
    iColorTable.append(QColor(0, 210, 0).rgb());
    iColorTable.append(QColor(210, 210, 0).rgb());
    iColorTable.append(QColor(0, 0, 240).rgb());
    iColorTable.append(QColor(210, 0, 210).rgb());
    iColorTable.append(QColor(0, 210, 210).rgb());
    iColorTable.append(QColor(235, 235, 235).rgb());

    //bright
    iColorTable.append(QColor(127, 127, 127).rgb());
    iColorTable.append(QColor(255, 0, 0).rgb());
    iColorTable.append(QColor(0, 255, 0).rgb());
    iColorTable.append(QColor(255, 255, 0).rgb());
    iColorTable.append(QColor(92, 92, 255).rgb());
    iColorTable.append(QColor(255, 0, 255).rgb());
    iColorTable.append(QColor(0, 255, 255).rgb());
    iColorTable.append(QColor(255, 255, 255).rgb());

    //colour cube
    for (int r = 0x00; r < 0x100; ) {
        for (int g = 0x00; g < 0x100; ) {
            for (int b = 0x00; b < 0x100; ) {
                iColorTable.append(QColor(r, g, b).rgb());
                b += b ? 0x28 : 0x5f;
            }
            g += g ? 0x28 : 0x5f;
        }
        r += r ? 0x28 : 0x5f;
    }

    //greyscale ramp
    for (int i = 0, g = 8; i < 24; i++, g += 10)
        iColorTable.append(QColor(g, g, g).rgb());

    if(iColorTable.size() != 256)
        qFatal("invalid color table");

    defaultFgColor = iColorTable[7];
    defaultBgColor = iColorTable[0];

    zeroChar.c = ' ';
    zeroChar.bgColor = defaultBgColor;
    zeroChar.fgColor = defaultFgColor;
    zeroChar.attrib = TermChar::NoAttributes;

    escape = -1;

    iTermAttribs.currentFgColor = defaultFgColor;
    iTermAttribs.currentBgColor = defaultBgColor;
    iTermAttribs.currentAttrib = TermChar::NoAttributes;
    iTermAttribs.cursorPos = QPoint(0,0);
    iMarginBottom = 0;
    iMarginTop = 0;

    resetBackBufferScrollPos();
    clearSelection();

    iTermAttribs_saved = iTermAttribs;
    iTermAttribs_saved_alt = iTermAttribs;

    resetTerminal();
}

void Terminal::init()
{
    m_pty = new PtyIFace(this, m_charset, m_terminalEnvironment, m_terminalCommand, this);
    if (m_pty->failed())
        qFatal("pty failure");
    connect(m_pty, SIGNAL(dataAvailable()), this, SLOT(onDataAvailable()));
    connect(m_pty, SIGNAL(hangupReceived()), this, SIGNAL(hangupReceived()));
}

void Terminal::onDataAvailable()
{
    if (m_dispatch_timer)
        return;

    // instantly dispatch
    insertInBuffer(m_pty->takeData());

    // ... but now, wait a while, so we don't constantly reupdate.
    m_dispatch_timer = startTimer(3);
}

void Terminal::timerEvent(QTimerEvent *)
{
    killTimer(m_dispatch_timer);
    m_dispatch_timer = 0;
    insertInBuffer(m_pty->takeData());
}

void Terminal::setCursorPos(QPoint pos)
{
    if( iTermAttribs.cursorPos != pos ) {
        int tlimit = 1;
        int blimit = iTermSize.height();
        if(iTermAttribs.originMode) {
            tlimit = iMarginTop;
            blimit = iMarginBottom;
        }

        if(pos.x() < 1)
            pos.setX(1);
        if(pos.x() > iTermSize.width()+1)
            pos.setX(iTermSize.width()+1);
        if(pos.y() < tlimit)
            pos.setY(tlimit);
        if(pos.y() > blimit)
            pos.setY(blimit);

        iTermAttribs.cursorPos=pos;
        if(iEmitCursorChangeSignal)
            emit cursorPosChanged(pos);
    }
}

QPoint Terminal::cursorPos()
{
    return iTermAttribs.cursorPos;
}

bool Terminal::showCursor()
{
    if(iBackBufferScrollPos != 0)
        return false;

    return iShowCursor;
}

const TerminalBuffer &Terminal::buffer() const
{
    if(iUseAltScreenBuffer)
        return iAltBuffer;

    return iBuffer;
}

TerminalBuffer &Terminal::buffer()
{
    if(iUseAltScreenBuffer)
        return iAltBuffer;

    return iBuffer;
}

void Terminal::setTermSize(QSize size)
{
    if( iTermSize != size ) {
        iMarginTop = 1;
        iMarginBottom = size.height();
        iTermSize=size;

        resetTabs();

        emit termSizeChanged(size.height(), size.width());
    }
}

void Terminal::putString(QString str)
{
    str.replace("\\r", "\r");
    str.replace("\\n", "\n");
    str.replace("\\e", QChar('\e'));
    str.replace("\\b", "\b");
    str.replace("\\t", "\t");

    //hex
    while(str.indexOf("\\x") != -1) {
        int i = str.indexOf("\\x")+2;
        QString num;
        while(num.length() < 2 && str.length()>i && charIsHexDigit(str.at(i))) {
            num.append(str.at(i));
            i++;
        }
        str.remove(i-2-num.length(), num.length()+2);
        bool ok;
        str.insert(i-2-num.length(), QChar(num.toInt(&ok,16)));
    }
    //octal
    while(str.indexOf("\\0") != -1) {
        int i = str.indexOf("\\0")+2;
        QString num;
        while(num.length() < 3 && str.length()>i &&
              (str.at(i).toLatin1() >= 48 && str.at(i).toLatin1() <= 55)) { //accept only 0-7
            num.append(str.at(i));
            i++;
        }
        str.remove(i-2-num.length(), num.length()+2);
        bool ok;
        str.insert(i-2-num.length(), QChar(num.toInt(&ok,8)));
    }

    m_pty->writeTerm(str);
}

void Terminal::keyPress(int key, int modifiers, const QString& text)
{
    QString toWrite;

    if (key > 0xFFFF) {
        int modcode = (modifiers & Qt::ShiftModifier ? 1 : 0) |
                      (modifiers & Qt::AltModifier ? 2 : 0) |
                      (modifiers & MyControlModifier? 4 : 0);

        if (modcode == 0) {
            QString fmt;
            char cursorModif='[';
            if(iAppCursorKeys)
                cursorModif = 'O';

            if( key==Qt::Key_Up ) fmt = QString("%2%1A").arg(cursorModif);
            if( key==Qt::Key_Down ) fmt = QString("%2%1B").arg(cursorModif);
            if( key==Qt::Key_Right ) fmt = QString("%2%1C").arg(cursorModif);
            if( key==Qt::Key_Left ) fmt = QString("%2%1D").arg(cursorModif);
            if( key==Qt::Key_PageUp ) fmt = "%1[5~";
            if( key==Qt::Key_PageDown ) fmt = "%1[6~";
            if( key==Qt::Key_Home ) fmt = "%1OH";
            if( key==Qt::Key_End ) fmt = "%1OF";
            if( key==Qt::Key_Insert ) fmt = "%1[2~";
            if( key==Qt::Key_Delete ) fmt = "%1[3~";

            if( key==Qt::Key_F1 ) fmt = "%1OP";
            if( key==Qt::Key_F2 ) fmt = "%1OQ";
            if( key==Qt::Key_F3 ) fmt = "%1OR";
            if( key==Qt::Key_F4 ) fmt = "%1OS";
            if( key==Qt::Key_F5 ) fmt = "%1[15~";
            if( key==Qt::Key_F6 ) fmt = "%1[17~";
            if( key==Qt::Key_F7 ) fmt = "%1[18~";
            if( key==Qt::Key_F8 ) fmt = "%1[19~";
            if( key==Qt::Key_F9 ) fmt = "%1[20~";
            if( key==Qt::Key_F10 ) fmt = "%1[21~";
            if( key==Qt::Key_F11 ) fmt = "%1[23~";
            if( key==Qt::Key_F12 ) fmt = "%1[24~";

            if (!fmt.isEmpty())
                toWrite += fmt.arg('\e');

        } else {
            QString fmt;
            char modChar = '1' + modcode;

            if( key==Qt::Key_Up ) fmt = "%1[1;%2A";
            if( key==Qt::Key_Down ) fmt = "%1[1;%2B";
            if( key==Qt::Key_Right ) fmt = "%1[1;%2C";
            if( key==Qt::Key_Left ) fmt = "%1[1;%2D";
            if( key==Qt::Key_PageUp ) fmt = "%1[5;%2~";
            if( key==Qt::Key_PageDown ) fmt = "%1[6;%2~";
            if( key==Qt::Key_Home ) fmt = "%1[1;%2H";
            if( key==Qt::Key_End ) fmt = "%1[1;%2F";
            if( key==Qt::Key_Insert ) fmt = "%1[2;%2~";
            if( key==Qt::Key_Delete ) fmt = "%1[3;%2~";

            if( key==Qt::Key_F1 ) fmt = "%1[1;%2P";
            if( key==Qt::Key_F2 ) fmt = "%1[1;%2Q";
            if( key==Qt::Key_F3 ) fmt = "%1[1;%2R";
            if( key==Qt::Key_F4 ) fmt = "%1[1;%2S";
            if( key==Qt::Key_F5 ) fmt = "%1[15;%2~";
            if( key==Qt::Key_F6 ) fmt = "%1[17;%2~";
            if( key==Qt::Key_F7 ) fmt = "%1[18;%2~";
            if( key==Qt::Key_F8 ) fmt = "%1[19;%2~";
            if( key==Qt::Key_F9 ) fmt = "%1[20;%2~";
            if( key==Qt::Key_F10 ) fmt = "%1[21;%2~";
            if( key==Qt::Key_F11 ) fmt = "%1[23;%2~";
            if( key==Qt::Key_F12 ) fmt = "%1[24;%2~";

            if (!fmt.isEmpty())
                toWrite += fmt.arg('\e').arg(modChar);

        }

        if( key==Qt::Key_Enter || key==Qt::Key_Return ) {
            if ( (modifiers & (Qt::ShiftModifier | MyControlModifier)) ==
                (Qt::ShiftModifier | MyControlModifier) )
                toWrite += QChar(0x9E);
            else if (modifiers & MyControlModifier)
                toWrite += QChar(0x1E); // ^^
            else if (modifiers & Qt::ShiftModifier)
                toWrite += "\n";
            else if(iNewLineMode)
                toWrite += "\r\n";
            else
                toWrite += "\r";
        }
        if( key==Qt::Key_Backspace ) {
            if ( (modifiers & (Qt::ShiftModifier | MyControlModifier)) ==
                (Qt::ShiftModifier | MyControlModifier) )
                toWrite += QChar(0x9F);
            else if (modifiers & MyControlModifier)
                toWrite += QChar(0x1F); // ^_
            else
                toWrite += "\x7F";
        }
        if( key==Qt::Key_Tab || key==Qt::Key_Backtab ) {
            if ( key == Qt::Key_Backtab ) modifiers |= Qt::ShiftModifier;
            if (modifiers & MyControlModifier) {
                char modChar = '5' + (modifiers & Qt::ShiftModifier ? 1 : 0);
                toWrite += QString("%1[1;%2I").arg('\e').arg(modChar);
            } else if (modifiers & Qt::ShiftModifier) {
                toWrite += QString("%1[Z").arg('\e');
            } else {
                toWrite += "\t";
            }
        }

        if( key==Qt::Key_Escape ) {
            if (modifiers & Qt::ShiftModifier)
                toWrite += QChar(0x9B);
            else
                toWrite += QString(1,'\e');
        }

        if (!toWrite.isEmpty()) {
            resetBackBufferScrollPos();
            if (!toWrite.startsWith('\e')) {
                // Only affect selection if not writing escape codes
                clearSelection();
            }

            m_pty->writeTerm(toWrite);
        }

        return;
    }

    QChar c(key);

    if (c.isLetter()) {
        c = ((modifiers & Qt::ShiftModifier) != 0) ? c.toUpper() : c.toLower();
    }

    if((modifiers & Qt::AltModifier) != 0) {
        toWrite.append('\e');
    }

    if ((modifiers & MyControlModifier) != 0) {
        char asciiVal = c.toUpper().toLatin1();

        if (asciiVal >= 0x41 && asciiVal <= 0x5f) {
            // Turn uppercase characters into their control code equivalent
            toWrite.append(asciiVal - 0x40);
        } else {
            qWarning() << "Ctrl+" << c << " does not translate into a control code";
        }
    } else {
        if (text.isEmpty()) {
            toWrite.append(c);
        } else {
            toWrite.append(text);
        }
    }

    resetBackBufferScrollPos();
    clearSelection();
    m_pty->writeTerm(toWrite);
    return;
}

void Terminal::insertInBuffer(const QString& chars)
{
    if (iTermSize.isNull())
        return;

    iEmitCursorChangeSignal = false;

    for(int i=0; i<chars.size(); i++) {
        const QChar &ch = chars.at(i);
        switch (ch.toLatin1()) {
        case '\n':
        case 11: // vertical tab
        case 12: // form feed
            if(cursorPos().y()==iMarginBottom) {
                scrollFwd(1);
                if(iNewLineMode)
                    setCursorPos(QPoint(1,cursorPos().y()));
            }
            else if(cursorPos().x() <= columns()) // ignore newline after <termwidth> cols (terminfo: xenl)
            {
                if(iNewLineMode)
                    setCursorPos(QPoint(1,cursorPos().y()+1));
                else
                    setCursorPos(QPoint(cursorPos().x(), cursorPos().y()+1));
            }
            break;
        case '\r':
            setCursorPos(QPoint(1,cursorPos().y()));
            break;
        case '\b':
        case 127: // del
            // only move cursor, don't actually erase.
            setCursorPos(QPoint(cursorPos().x()-1,cursorPos().y()));
            break;
        case '\a': // BEL
            if(escape==']') {  // BEL also ends OSC sequence
                escape=-1;
                oscSequence(oscSeq);
                oscSeq.clear();
            } else {
                emit visualBell();
            }
            break;
        case '\t':
            forwardTab();
            break;
        case 14: // SI
        case 15: // SO
            // related to character set... ignore
            break;
        default:
            if( escape>=0 ) {
                if( escape==0 && (ch.toLatin1()=='[') ) {
                    escape='['; //ansi sequence
                    escSeq += ch;
                }
                else if( escape==0 && (ch.toLatin1()==']') ) {
                    escape=']'; //osc sequence
                    oscSeq += ch;
                }
                else if( escape==0 && multiCharEscapes.contains(ch.toLatin1())) {
                    escape = ch.toLatin1();
                    escSeq += ch;
                }
                else if( escape==0 && ch.toLatin1()=='\\' ) {  // ESC\ also ends OSC sequence
                    escape=-1;
                    oscSequence(oscSeq);
                    oscSeq.clear();
                }
                else if (ch.toLatin1()=='\e') {
                    escape = 0;
                }
                else if( escape=='[' || multiCharEscapes.contains(escape) ) {
                    escSeq += ch;
                }
                else if( escape==']' ) {
                    oscSeq += ch;
                }
                else if( multiCharEscapes.contains(escape) ) {
                    escSeq += ch;
                }
                else {
                    escControlChar(QByteArray(1,ch.toLatin1()));
                    escape=-1;
                }

                if( escape=='[' && ch.toLatin1() >= 64 && ch.toLatin1() <= 126 && ch.toLatin1() != '[' ) {
                    ansiSequence(escSeq);
                    escape=-1;
                    escSeq.clear();
                }
                if( multiCharEscapes.contains(escape) && escSeq.length()>=2 ) {
                    escControlChar(escSeq);
                    escape=-1;
                    escSeq.clear();
                }
            } else {
                if (ch.isPrint())
                    insertAtCursor(ch, !iReplaceMode);
                else if (ch.toLatin1()=='\e')
                    escape=0;
            }
            break;
        }
    }

    iEmitCursorChangeSignal = true;
    emit displayBufferChanged();
}

void Terminal::insertAtCursor(QChar c, bool overwriteMode, bool advanceCursor)
{
    if(cursorPos().x() > iTermSize.width() && advanceCursor) {
        if(iTermAttribs.wrapAroundMode) {
            if(cursorPos().y()>=iMarginBottom) {
                scrollFwd(1);
                setCursorPos(QPoint(1, cursorPos().y()));
            } else {
                setCursorPos(QPoint(1, cursorPos().y()+1));
            }
        } else {
            setCursorPos(QPoint(iTermSize.width(), cursorPos().y()));
        }
    }

    auto &line = currentLine();
    while (line.size() < cursorPos().x() )
        line.append(zeroChar);

    if(!overwriteMode)
        line.insert(cursorPos().x()-1,zeroChar);

    line[cursorPos().x()-1].c = c;
    line[cursorPos().x()-1].fgColor = iTermAttribs.currentFgColor;
    line[cursorPos().x()-1].bgColor = iTermAttribs.currentBgColor;
    line[cursorPos().x()-1].attrib = iTermAttribs.currentAttrib;

    if (advanceCursor) {
        setCursorPos(QPoint(cursorPos().x()+1,cursorPos().y()));
    }
}

void Terminal::deleteAt(QPoint pos)
{
    clearAt(pos);
    buffer()[pos.y()-1].removeAt(pos.x()-1);
}

void Terminal::clearAt(QPoint pos)
{
    if(pos.y() <= 0 || pos.y()-1 > buffer().size() ||
            pos.x() <= 0 || pos.x()-1 > buffer()[pos.y()-1].size())
    {
        qDebug() << "warning: trying to clear char out of bounds";
        return;
    }

    // just in case...
    while(buffer().size() < pos.y())
        buffer().append(TerminalLine());
    while(buffer()[pos.y()-1].size() < pos.x() )
        buffer()[pos.y()-1].append(zeroChar);

    buffer()[pos.y()-1][pos.x()-1] = zeroChar;
}

void Terminal::eraseLineAtCursor(int from, int to)
{
    if(from==-1 && to==-1) {
        currentLine().clear();
        return;
    }
    if(from < 1)
        from=1;
    from--;

    if(to < 1 || to > currentLine().size())
        to=currentLine().size();
    to--;

    if(from>to)
        return;

    for(int i=from; i<=to; i++) {
        currentLine()[i] = zeroChar;
    }
}

void Terminal::clearAll(bool wholeBuffer)
{
    clearSelection();
    if(wholeBuffer) {
        backBuffer().clear();
        resetBackBufferScrollPos();
        clearSelection();
    }
    buffer().clear();
    setCursorPos(QPoint(1,1));
}

void Terminal::backwardTab()
{
    if(cursorPos().y() <= iTabStops.size()) {
        for(int i=iTabStops[cursorPos().y()-1].count()-1; i>=0; i--) {
            if(iTabStops[cursorPos().y()-1][i] < cursorPos().x()) {
                setCursorPos(QPoint( iTabStops[cursorPos().y()-1][i], cursorPos().y() ));
                break;
            }
        }
    }
}

void Terminal::forwardTab()
{
    if(cursorPos().y() <= iTabStops.size()) {
        for(int i=0; i<iTabStops[cursorPos().y()-1].count(); i++) {
            if(iTabStops[cursorPos().y()-1][i] > cursorPos().x()) {
                setCursorPos(QPoint( iTabStops[cursorPos().y()-1][i], cursorPos().y() ));
                break;
            }
        }
    }
}

void Terminal::ansiSequence(const QString& seq)
{
    if(seq.length() <= 1 || seq.at(0)!='[')
        return;

    QChar cmdChar = seq.at(seq.length()-1);
    QString extra;
    QList<int> params;

    int x=1;
    while(x<seq.length()-1 && !QChar(seq.at(x)).isNumber())
        x++;

    QList<QString> tmp = seq.mid(x,seq.length()-x-1).split(';');
    foreach(QString b, tmp) {
        bool ok=false;
        int t = b.toInt(&ok);
        if(ok) {
            params.append(t);
        }
    }
    if(x>1)
        extra = seq.mid(1,x-1);

    bool unhandled = false;

    switch(cmdChar.toLatin1())
    {
    case 'A': //cursor up
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), qMax(iMarginTop, cursorPos().y()-params.at(0)) ));
        break;
    case 'B': //cursor down
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), qMin(iMarginBottom, cursorPos().y()+params.at(0)) ));
        break;
    case 'C': //cursor fwd
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( qMin(iTermSize.width(),cursorPos().x()+params.at(0)), cursorPos().y() ));
        break;
    case 'D': //cursor back
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( qMax(1,cursorPos().x()-params.at(0)), cursorPos().y() ));
        break;
    case 'E': //cursor next line
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( 1, qMin(iMarginBottom, cursorPos().y()+params.at(0)) ));
        break;
    case 'F': //cursor prev line
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( 1, qMax(iMarginTop, cursorPos().y()-params.at(0)) ));
        break;
    case 'G': //go to column
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( params.at(0), cursorPos().y() ));
        break;
    case 'H': //cursor pos
    case 'f': //cursor pos
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        while(params.count()<2)
            params.append(1);
        if (iTermAttribs.originMode)
            setCursorPos(QPoint( params.at(1), params.at(0)+iMarginTop-1 ));
        else
            setCursorPos(QPoint( params.at(1), params.at(0) ));
        break;
    case 'J':
        unhandled = handleDECSED(params, extra);
        break;
    case 'K':
        unhandled = handleEL(params, extra);
        break;
    case 'X':
        unhandled = handleECH(params, extra);
        break;

    case 'I':
        if(!extra.isEmpty() || (params.count()>1)) {
            unhandled=true;
            break;
        }
        if (params.count()==0) {
            params.append(1);
        }
        for (int i=0;i<params.at(0);i++) {
            forwardTab();
        }
        break;

    case 'Z':
        if(!extra.isEmpty() || (params.count()>1)) {
            unhandled=true;
            break;
        }
        if (params.count()==0) {
            params.append(1);
        }
        for (int i=0;i<params.at(0);i++) {
            backwardTab();
        }
        break;

    case 'L':  // insert lines
        unhandled = handleIL(params, extra);
        break;
    case 'M':  // delete lines
        unhandled = handleDL(params, extra);
        break;
    case 'P': // delete characters
        unhandled = handleDCH(params, extra);
        break;
    case '@': // insert blank characters
        unhandled = handleICH(params, extra);
        break;

    case 'S':  // scroll up n lines
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        scrollFwd(params.at(0));
        break;
    case 'T':  // scroll down n lines
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        scrollBack(params.at(0));
        break;

    case 'c': // vt100 identification
        if(params.count()==0)
            params.append(0);
        if(params.count()==1 && params.at(0)==0) {
            QString toWrite = QString("%1[?1;2c").arg('\e').toLatin1();
            m_pty->writeTerm(toWrite);
        } else unhandled=true;
        break;

    case 'd': //go to row
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), params.at(0) ));
        break;

    case 'g': //tab stop manipulation
        if(params.count()==0)
            params.append(0);
        if(params.at(0)==0 && extra=="") {  //clear tab at current position
            if(cursorPos().y() <= iTabStops.size()) {
                int idx = iTabStops[cursorPos().y()-1].indexOf(cursorPos().x());
                if(idx != -1)
                    iTabStops[cursorPos().y()-1].removeAt(idx);
            }
        }
        else if(params.at(0)==3 && extra=="") {  //clear all tabs
            iTabStops.clear();
        }
        break;

    case 'n':
        if(params.count()>=1 && params.at(0)==6 && extra=="") {  // write cursor pos
            QString toWrite = QString("%1[%2;%3R").arg('\e').arg(cursorPos().y()).arg(cursorPos().x()).toLatin1();
            m_pty->writeTerm(toWrite);
        } else unhandled=true;
        break;

    case 'p':
        if(extra=="!") {  // reset terminal
            resetTerminal();
        } else unhandled=true;
        break;

    case 's': //save cursor
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        iTermAttribs_saved = iTermAttribs;
        break;
    case 'u': //restore cursor
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        iTermAttribs = iTermAttribs_saved;
        break;

    case 'm': //graphics mode
        if (params.count() == 0)
            params.append(0);
        handleSGR(params, extra);
        break;

    case 'h':
        for (int i = 0; i < params.size(); ++i) {
            handleMode(params.at(i), true, extra);
        }
        break;

    case 'l':
        for (int i = 0; i < params.size(); ++i) {
            handleMode(params.at(i), false, extra);
        }
        break;

    case 'r':  // scrolling region
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count() < 2) {
            while(params.count() < 2)
                params.append(1);
            params[0] = 1;
            params[1] = iTermSize.height();
        }
        if(params.at(0) < 1)
            params[0] = 1;
        if(params.at(1) > iTermSize.height())
            params[1] = iTermSize.height();
        iMarginTop = params.at(0);
        iMarginBottom = params.at(1);
        if(iMarginTop >= iMarginBottom) {
            //invalid scroll region
            if(iMarginTop == iTermSize.height()) {
                iMarginTop = iMarginBottom - 1;
            } else {
                iMarginBottom = iMarginTop + 1;
            }
        }
        setCursorPos(QPoint( 1, iMarginTop ));
        break;

    default:
        unhandled=true;
        break;
    }

    if (unhandled)
        qDebug() << "unhandled ansi sequence " << cmdChar << params << extra;
}

void Terminal::handleMode(int mode, bool set, const QString &extra)
{
    if (extra == "?") {
        switch (mode) {
        case 1:
            iAppCursorKeys = set;
            break;
        case 3: // column mode
            // not supported, just clear screen, move cursor home & reset scrolling region
            clearAll();
            resetTabs();
            iMarginTop = 1;
            iMarginBottom = iTermSize.height();
            break;
        case 5: {
            // inverse video (DECSCNM)
            m_inverseVideoMode = set;
            break;
        }
        case 6: // origin mode
            iTermAttribs.originMode = set;
            break;
        case 7:
            iTermAttribs.wrapAroundMode = set;
            break;
        case 12: // start blinking cursor
            break;
        case 25: // show cursor
            iShowCursor = set;
            break;
        case 1049: // use alt screen buffer and save cursor
            if (set) {
                iTermAttribs_saved_alt = iTermAttribs;
                iUseAltScreenBuffer = true;
                iMarginTop = 1;
                iMarginBottom = iTermSize.height();
                resetBackBufferScrollPos();
                clearSelection();
                clearAll();
            } else {
                iUseAltScreenBuffer = false;
                iTermAttribs = iTermAttribs_saved_alt;
                iMarginBottom = iTermSize.height();
                iMarginTop = 1;
                resetBackBufferScrollPos();
                clearSelection();
            }
            resetTabs();
            emit displayBufferChanged();
            break;
        case 2004: // bracketed paste mode
            m_bracketedPasteMode = set;
            break;
        default:
            qDebug() << "Unhandled DEC mode " << mode << set << extra;
        }
    } else if (extra == "") {
        switch (mode) {
        case 4:
            iReplaceMode = set;
            break;
        case 20:
            iNewLineMode = set;
            break;
        default:
            qDebug() << "Unhandled DEC mode " << mode << set << extra;
        }
    } else {
        qDebug() << "Unhandled DEC mode " << mode << set << extra;
    }
}

bool Terminal::handleIL(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty()) {
        return false;
    }
    if(cursorPos().y() < iMarginTop || cursorPos().y() > iMarginBottom)
        return true;

    int p = params.count() < 0 ? 1 : params.at(0);
    if (p == 0) {
        p = 1;
    }

    if(p > iMarginBottom-cursorPos().y())
        scrollBack(iMarginBottom-cursorPos().y(), cursorPos().y());
    else
        scrollBack(p, cursorPos().y());
    setCursorPos(QPoint(1,cursorPos().y()));
    return true;
}

bool Terminal::handleDL(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty()) {
        return false;
    }
    if(cursorPos().y() < iMarginTop || cursorPos().y() > iMarginBottom)
        return true;

    int p = params.count() < 0 ? 1 : params.at(0);
    if (p == 0) {
        p = 1;
    }

    if(p > iMarginBottom-cursorPos().y())
        scrollFwd(iMarginBottom-cursorPos().y(), cursorPos().y());
    else
        scrollFwd(p, cursorPos().y());
    setCursorPos(QPoint(1,cursorPos().y()));
    return true;
}

bool Terminal::handleDCH(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty()) {
        return false;
    }

    int p = params.count() < 0 ? 1 : params.at(0);
    if (p == 0) {
        p = 1;
    }

    for(int i=0; i<p; i++)
        deleteAt(cursorPos());
    return true;
}

bool Terminal::handleICH(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty()) {
        return false;
    }

    int p = params.count() < 0 ? 1 : params.at(0);
    if (p == 0) {
        p = 1;
    }

    for(int i=1; i<=p; i++)
        insertAtCursor(zeroChar.c, false, false);
    return true;
}

// Erase in Display (DECSED)
bool Terminal::handleDECSED(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty() && extra!="?") {
        return false;
    }

    if(params.count()>=1 && params.at(0)==1) {
        eraseLineAtCursor(1,cursorPos().x());
        for(int i=0; i<cursorPos().y()-1; i++) {
            buffer()[i].clear();
        }
    } else if(params.count()>=1 && params.at(0)==2) {
        clearAll();
    } else {
        eraseLineAtCursor(cursorPos().x());
        for(int i=cursorPos().y(); i<buffer().size(); i++)
            buffer()[i].clear();
    }

    return true;
}

// Erase in Line (EL)
bool Terminal::handleEL(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty() && extra!="?") {
        return false;
    }
    if(params.count()>=1 && params.at(0)==1) {
        eraseLineAtCursor(1,cursorPos().x());
    }
    else if(params.count()>=1 && params.at(0)==2) {
        currentLine().clear();
    } else {
        eraseLineAtCursor(cursorPos().x());
    }
    return true;
}

// Erase Characters (ECH)
bool Terminal::handleECH(const QList<int> &params, const QString &extra)
{
    if(!extra.isEmpty() || (params.count()>1)) {
        return false;
    }

    int p = params.isEmpty() ? 1 : params.at(0);
    eraseLineAtCursor(cursorPos().x(),cursorPos().x()+(p ? p - 1 : 0));
    return true;
}

void Terminal::handleSGR(const QList<int> &params, const QString &extra)
{
// I think this check was wrong. At least vttest sends 5; 7; sometimes.
//    if(!extra.isEmpty()) {
//        qDebug() << "got SGR with empty extra, params: " << params;
//        return;
//    }

    int pidx = 0;

    while (pidx < params.count()) {
        int p = params.at(pidx++);
        switch (p) {
        // Consider supporting:
        // 2 (faint)
        // 8 (invisible)
        // 9 (crossed out -- what is this?)
        // 21 (double underline)
        // 22 (normal, not faint)
        // 28 (visible)
        // 29 (not crossed out)

        case 0:
            iTermAttribs.currentFgColor = defaultFgColor;
            iTermAttribs.currentBgColor = defaultBgColor;
            iTermAttribs.currentAttrib = TermChar::NoAttributes;
            break;
        case 1:
            iTermAttribs.currentAttrib |= TermChar::BoldAttribute;
            break;
        case 3:
            iTermAttribs.currentAttrib |= TermChar::ItalicAttribute;
            break;
        case 4:
            iTermAttribs.currentAttrib |= TermChar::UnderlineAttribute;
            break;
        case 5:
            iTermAttribs.currentAttrib |= TermChar::BlinkAttribute;
            break;
        case 7:
            iTermAttribs.currentAttrib |= TermChar::NegativeAttribute;
            break;
        case 22:
            iTermAttribs.currentAttrib &= ~TermChar::BoldAttribute;
            break;
        case 23:
            iTermAttribs.currentAttrib &= ~TermChar::ItalicAttribute;
            break;
        case 24:
            iTermAttribs.currentAttrib &= ~TermChar::UnderlineAttribute;
            break;
        case 25:
            iTermAttribs.currentAttrib &= ~TermChar::BlinkAttribute;
            break;
        case 27:
            iTermAttribs.currentAttrib &= ~TermChar::NegativeAttribute;
            break;

        case 30: // fg black
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            if (iTermAttribs.currentFgColor & TermChar::BoldAttribute)
                p += 8;
            iTermAttribs.currentFgColor = iColorTable[p-30];
            break;

        case 39: // fg default
            iTermAttribs.currentFgColor = defaultFgColor;
            break;

        case 40: // bg black
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            iTermAttribs.currentBgColor = iColorTable[p-40];
            break;

        case 49: // bg default
            iTermAttribs.currentBgColor = defaultBgColor;
            break;


        case 90: // fg black, bold/bright, nonstandard
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            iTermAttribs.currentFgColor = iColorTable[p-90+8];
            break;

        case 100: // fg black, bold/bright, nonstandard
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            iTermAttribs.currentFgColor = iColorTable[p-100+8];
            break;

        case 38:
        case 48:
            // xterm 256-colour support
            if (params.count() - pidx < 2) {
                qDebug() << "got invalid extended SGR: " << params << extra;
                continue;
            }
            if(params[pidx] == 5 && params[pidx+1] >= 0 && params[pidx+1] <= 255) {
                if(p == 38) {
                    int cidx = params[pidx+1];
                    // Only apply bold attribute for standard 16-color; 256-color doesn't have bold
                    if (cidx < 9 && iTermAttribs.currentAttrib & TermChar::BoldAttribute)
                        cidx += 8;
                    iTermAttribs.currentFgColor = iColorTable[cidx];
                }
                else
                    iTermAttribs.currentBgColor = iColorTable[params[pidx+1]];

                pidx += 2; // eat the '5' and color index
            } else if (params[pidx] == 2 && params.count() - pidx >= 5) {
                if (p == 38)
                    iTermAttribs.currentFgColor = QColor(params.at(pidx+1), params.at(pidx+2), params.at(pidx+3)).rgb();
                else
                    iTermAttribs.currentBgColor = QColor(params.at(pidx+1), params.at(pidx+2), params.at(pidx+3)).rgb();

                pidx += 4; // '2' and 3 colors.
            } else {
                qDebug() << "got malformed extended SGR: " << params << extra;
                continue;
            }
            break;
        }
    }
}

void Terminal::oscSequence(const QString& seq)
{
    if(seq.length() <= 1 || seq.at(0)!=']')
        return;

    if (seq.length() >= 3) {
        if (seq.at(0) == ']') {
            if ((seq.at(1) == '0' || seq.at(1) == '2') && seq.at(2) == ';') {
                // set window title
                emit windowTitleChanged(seq.mid(3));
                return;
            } else if (seq.at(1) == '6' && seq.at(2) == ';') {
                // iTerm2 proprietary:
                // Set window title and tab chrome background color
                // Ignore for the time being...
                return;
            }
        }
    }

    if (seq.length() >= 4) {
        if (seq.at(0) == ']') {
            if (seq.at(1) == '1' && seq.at(2) == '3' && seq.at(3) == '3' && seq.at(4) == ';') {
                // iTerm2 proprietary(?)
                // Prompt state stuff, related to shell integration
                // Ignore for the time being...
                return;
            }
        }
    }

    if (seq.length() >= 5) {
        if (seq.at(0) == ']') {
            if (seq.at(1) == '1' && seq.at(2) == '3' && seq.at(3) == '3' && seq.at(4) == '7' && seq.at(5) == ';') {
                // iTerm2 proprietary, various stuff, shell integration and more.
                // Ignore for the time being...
                return;
            }
        }
    }

    qDebug() << "unhandled OSC" << seq;
}

void Terminal::escControlChar(const QString& seq)
{
    QChar ch;

    if(seq.length()==1) {
        ch = seq.at(0);
    } else if (seq.length()>1 ){ // control sequences longer than 1 characters
        if( seq.at(0) == '(' || seq.at(0)==')' ) // character set, ignore this for now...
            return;
        if( seq.at(0) == '#' && seq.at(1)=='8' ) { // test mode, fill screen with 'E'
            clearAll(true);
            for (int i = 0; i < rows(); i++) {
                TerminalLine line;
                for(int j = 0; j < columns(); j++) {
                    TermChar c = zeroChar;
                    c.c = 'E';
                    line.append(c);
                }
                buffer().append(line);
            }
            return;
        }
    }

    if(ch.toLatin1()=='7') { //save cursor
        iTermAttribs_saved = iTermAttribs;
    }
    else if(ch.toLatin1()=='8') { //restore cursor
        iTermAttribs = iTermAttribs_saved;
    }
    else if(ch.toLatin1()=='>' || ch.toLatin1()=='=') { //app keypad/normal keypad - ignore these for now...
    }

    else if(ch.toLatin1()=='H') {  // set a tab stop at cursor position
        while(iTabStops.size() < cursorPos().y())
            iTabStops.append(QVector<int>());

        iTabStops[cursorPos().y()-1].append(cursorPos().x());
        qSort(iTabStops[cursorPos().y()-1]);
    }
    else if(ch.toLatin1()=='D') {  // cursor down/scroll down one line
        scrollFwd(1, cursorPos().y());
    }
    else if(ch.toLatin1()=='M') {  // cursor up/scroll up one line
        scrollBack(1, cursorPos().y());
    }

    else if(ch.toLatin1()=='E') {  // new line
        if(cursorPos().y()==iMarginBottom) {
            scrollFwd(1);
            setCursorPos(QPoint(1,cursorPos().y()));
        } else {
            setCursorPos(QPoint(1,cursorPos().y()+1));
        }
    }
    else if(ch.toLatin1()=='c') {  // full reset
        resetTerminal();
    }
    else if(ch.toLatin1()=='g') {  // visual bell
        emit visualBell();
    }
    else {
        qDebug() << "unhandled escape code ESC" << seq;
    }
}

TerminalLine &Terminal::currentLine()
{
    while(buffer().size() <= cursorPos().y()-1)
        buffer().append(TerminalLine());

    if( cursorPos().y() >= 1 &&
            cursorPos().y() <= buffer().size() )
    {
        return buffer()[cursorPos().y()-1];
    }

    // we shouldn't get here
    return buffer()[buffer().size()-1];
}

const QStringList Terminal::printableLinesFromCursor(int lines)
{
    QStringList ret;

    int start = cursorPos().y() - lines;
    int end = cursorPos().y() + lines;

    for(int l=start-1; l<end; l++) {
        ret.append("");
        if(l >= 0 && l < buffer().size()) {
            for(int i=0; i<buffer()[l].size(); i++) {
                if(buffer()[l][i].c.isPrint())
                    ret[ret.size()-1].append(buffer()[l][i].c);
            }
        }
    }

    return ret;
}

void Terminal::trimBackBuffer()
{
    // ### this could be done better (removeN)
    while(backBuffer().size() > maxScrollBackLines) {
        backBuffer().removeAt(0);
    }
}

void Terminal::scrollBack(int lines, int insertAt)
{
    if(lines <= 0)
        return;

    adjustSelectionPosition(lines);

    bool useBackbuffer = true;
    if(insertAt==-1) {
        insertAt = iMarginTop;
        useBackbuffer = false;
    }
    insertAt--;

    while(lines>0) {
        if(!iUseAltScreenBuffer) {
            if(iBackBuffer.size()>0 && useBackbuffer)
                buffer().insert(insertAt, iBackBuffer.takeAt(iBackBuffer.size() - 1));
            else
                buffer().insert(insertAt, TerminalLine());
        } else {
            buffer().insert(insertAt, TerminalLine());
        }

        int rm = iMarginBottom;
        if(rm >= buffer().size())
            rm = buffer().size()-1;

        buffer().removeAt(rm);

        lines--;
    }
}

void Terminal::scrollFwd(int lines, int removeAt)
{
    if(lines <= 0)
        return;

    adjustSelectionPosition(-lines);

    if(removeAt==-1) {
        removeAt = iMarginTop;
    }
    removeAt--;

    while(buffer().size() < iMarginBottom)
        buffer().append(TerminalLine());

    while(lines>0) {
        buffer().insert(iMarginBottom, TerminalLine());

        if(!iUseAltScreenBuffer)
            iBackBuffer.append( buffer().takeAt(removeAt) );
        else
            buffer().removeAt(removeAt);

        lines--;
    }
    trimBackBuffer();
}

void Terminal::resetTerminal()
{
    iBuffer.clear();
    iAltBuffer.clear();
    iBackBuffer.clear();

    iTermAttribs.currentFgColor = defaultFgColor;
    iTermAttribs.currentBgColor = defaultBgColor;
    iTermAttribs.currentAttrib = TermChar::NoAttributes;
    iTermAttribs.cursorPos = QPoint(1,1);
    iTermAttribs.wrapAroundMode = true;
    iTermAttribs.originMode = false;

    iTermAttribs_saved = iTermAttribs;
    iTermAttribs_saved_alt = iTermAttribs;

    iMarginBottom = iTermSize.height();
    iMarginTop = 1;

    iShowCursor = true;
    iUseAltScreenBuffer = false;
    iAppCursorKeys = false;
    iReplaceMode = false;
    iNewLineMode = false;
    m_inverseVideoMode = false;
    m_bracketedPasteMode = false;

    resetBackBufferScrollPos();
    clearSelection();

    resetTabs();
    clearSelection();
}

void Terminal::resetTabs()
{
    iTabStops.clear();
    for(int i=0; i<iTermSize.height(); i++) {
        int tab=1;
        iTabStops.append(QVector<int>());
        while(tab <= iTermSize.width()) {
            iTabStops.last().append(tab);
            tab += 8;
        }
    }
}

void Terminal::paste(const QString &text)
{
    if(!text.isEmpty()) {
        resetBackBufferScrollPos();
        clearSelection();

        if (m_bracketedPasteMode) {
            m_pty->writeTerm(QString::fromLatin1("\e[200~"));
        }
        m_pty->writeTerm(text);
        if (m_bracketedPasteMode) {
            m_pty->writeTerm(QString::fromLatin1("\e[201~"));
        }
    }
}

const QStringList Terminal::grabURLsFromBuffer()
{
    QStringList ret;
    QByteArray buf;

    //backbuffer
    if (!iUseAltScreenBuffer
        || backBufferScrollPos() > 0)  //a lazy workaround: just grab everything when the buffer is being scrolled (TODO: make a proper fix)
    {
        for (int i=0; i<iBackBuffer.size(); i++) {
            for (int j=0; j<iBackBuffer[i].size(); j++) {
                if (iBackBuffer[i][j].c.isPrint())
                    buf.append(iBackBuffer[i][j].c);
                else if (iBackBuffer[i][j].c == 0)
                    buf.append(' ');
            }
            if (iBackBuffer[i].size() < iTermSize.width())
                buf.append(' ');
        }
    }

    //main buffer
    for (int i=0; i<buffer().size(); i++) {
        for (int j=0; j<buffer()[i].size(); j++) {
            if (buffer()[i][j].c.isPrint())
                buf.append(buffer()[i][j].c);
            else if (buffer()[i][j].c == 0)
                buf.append(' ');
        }
        if (buffer()[i].size() < iTermSize.width())
            buf.append(' ');
    }

    /* http://blog.mattheworiordan.com/post/13174566389/url-regular-expression-for-links-with-or-without-the */
    QRegularExpression re("("
                   "(" // brackets covering match for protocol (optional) and domain
                     "([A-Za-z]{3,9}:(?:\\/\\/)?)" // match protocol, allow in format http:// or mailto:
                     "(?:[\\-;:&=\\+\\$,\\w]+@)?" // allow something@ for email addresses
                     "[A-Za-z0-9\\.\\-]+" // anything looking at all like a domain, non-unicode domains
                     "|" // or instead of above
                     "(?:www\\.|[\\-;:&=\\+\\$,\\w]+@)" // starting with something@ or www.
                     "[A-Za-z0-9\\.\\-]+"   // anything looking at all like a domain
                   ")"
                   "(" // brackets covering match for path, query string and anchor
                     "(?:\\/[\\+~%\\/\\.\\w\\-]*)" // allow optional /path
                     "?\\?""?(?:[\\-\\+=&;%@\\.\\w]*)" // allow optional query string starting with ?
                     "#?(?:[\\.\\!\\/\\\\\\w]*)" // allow optional anchor #anchor
                   ")?" // make URL suffix optional
               ")");

    QRegularExpressionMatchIterator i = re.globalMatch(buf);
    while (i.hasNext()) {
        QRegularExpressionMatch match = i.next();
        QString word = match.captured(1);
        ret << word;
    }

    ret.removeDuplicates();
    return ret;
}

void Terminal::scrollBackBufferFwd(int lines)
{
    if (lines<=0)
        return;

    if (iUseAltScreenBuffer) {
        QString fmt;
        char cursorModif='[';
        if(iAppCursorKeys)
            cursorModif = 'O';

        fmt = QString("%2%1B").arg(cursorModif);
        m_pty->writeTerm(fmt.arg('\e'));
    } else {
        iBackBufferScrollPos -= lines;
        if(iBackBufferScrollPos < 0) {
            iBackBufferScrollPos = 0;
        } else {
            adjustSelectionPosition(-lines);
        }

        emit scrollBackBufferAdjusted(false);
    }
}

void Terminal::scrollBackBufferBack(int lines)
{
    if (lines<=0)
        return;

    if (iUseAltScreenBuffer) {
        QString fmt;
        char cursorModif='[';
        if(iAppCursorKeys)
            cursorModif = 'O';

        fmt = QString("%2%1A").arg(cursorModif);
        m_pty->writeTerm(fmt.arg('\e'));
    } else {
        iBackBufferScrollPos += lines;
        if (iBackBufferScrollPos > iBackBuffer.size()) {
            iBackBufferScrollPos = iBackBuffer.size();
        } else {
            adjustSelectionPosition(lines);
        }

        emit scrollBackBufferAdjusted(false);
    }
}

void Terminal::resetBackBufferScrollPos()
{
    if(iBackBufferScrollPos==0 && iSelection.isNull())
        return;

    if (!iUseAltScreenBuffer) {
        scrollBackBufferFwd(iBackBufferScrollPos);
    } else {
        iBackBufferScrollPos = 0;
        emit scrollBackBufferAdjusted(true);
    }
}

QString Terminal::selectedText() const
{
    if (selection().isNull())
        return QString();

    QString text;
    QString line;

    // backbuffer
    if (iBackBufferScrollPos > 0 && !iUseAltScreenBuffer) {
        int lineFrom = iBackBuffer.size() - iBackBufferScrollPos + selection().top() - 1;
        int lineTo = iBackBuffer.size() - iBackBufferScrollPos + selection().bottom() - 1;

        for (int i=lineFrom; i<=lineTo; i++) {
            if (i >= 0 && i < iBackBuffer.size()) {
                line.clear();
                int start = 0;
                int end = iBackBuffer[i].size()-1;
                if (i==lineFrom) {
                    start = selection().left()-1;
                }
                if (i==lineTo) {
                    end = selection().right()-1;
                }
                for (int j=start; j<=end; j++) {
                    if (j >= 0 && j < iBackBuffer[i].size() && iBackBuffer[i][j].c.isPrint())
                        line += iBackBuffer[i][j].c;
                }
                text += line.trimmed() + "\n";
            }
        }
    }

    // main buffer
    int lineFrom = selection().top()-1-iBackBufferScrollPos;
    int lineTo = selection().bottom()-1-iBackBufferScrollPos;
    for (int i=lineFrom; i<=lineTo; i++) {
        if (i >= 0 && i < buffer().size()) {
            line.clear();
            int start = 0;
            int end = buffer()[i].size()-1;
            if (i==lineFrom) {
                start = selection().left()-1;
            }
            if (i==lineTo) {
                end = selection().right()-1;
            }
            for (int j=start; j<=end; j++) {
                if (j >= 0 && j < buffer()[i].size() && buffer()[i][j].c.isPrint())
                    line += buffer()[i][j].c;
            }
            text += line.trimmed() + "\n";
        }
    }

    return text.trimmed();
}

void Terminal::adjustSelectionPosition(int lines)
{
    // adjust selection position when terminal contents move

    if (iSelection.isNull() || lines==0)
        return;

    int tx = iSelection.left();
    int ty = iSelection.top() + lines;
    int bx = iSelection.right();
    int by = iSelection.bottom() + lines;

    if (ty<1) {
        ty = 1;
        tx = 1;
    }
    if (by>iTermSize.height()) {
        by = iTermSize.height();
        bx = iTermSize.width();
    }
    if (by<1 || ty>iTermSize.height()) {
        clearSelection();
        return;
    }

    iSelection = QRect(QPoint(tx,ty), QPoint(bx,by));

    emit selectionChanged();
}

void Terminal::setSelection(QPoint start, QPoint end, bool selectionOngoing)
{
    if (start.y() > end.y())
        qSwap(start, end);
    if (start.y() == end.y() && start.x() > end.x())
        qSwap(start, end);

    if (start.x() < 1)
        start.rx() = 1;
    if (start.y() < 1)
        start.ry() = 1;
    if (end.x() > iTermSize.width())
        end.rx() = iTermSize.width();
    if (end.y() > iTermSize.height())
        end.ry() = iTermSize.height();

    iSelection = QRect(start, end);

    emit selectionChanged();

    if (!selectionOngoing) {
        emit selectionFinished();
    }
}

void Terminal::clearSelection()
{
    if (iSelection.isNull())
        return;

    iSelection = QRect();

    emit selectionFinished();
    emit selectionChanged();
}

int Terminal::rows() const
{
    return iTermSize.height();
}

int Terminal::columns() const
{
    return iTermSize.width();
}

QRect Terminal::selection() const
{
    return iSelection;
}
