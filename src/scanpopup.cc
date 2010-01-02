/* This file is (c) 2008-2009 Konstantin Isakov <ikm@users.berlios.de>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "scanpopup.hh"
#include "folding.hh"
#include "mouseover.hh"
#include "wstring_qt.hh"
#include <QUrl>
#include <QCursor>
#include <QPixmap>
#include <QBitmap>
#include <QMenu>
#include <QMouseEvent>
#include <QDesktopWidget>

using std::wstring;

/// We use different window flags under Windows and X11 due to slight differences
/// in their behavior on those platforms.
static Qt::WindowFlags popupWindowFlags =

#ifdef Q_WS_WIN
Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
#else
Qt::Popup
#endif
;

ScanPopup::ScanPopup( QWidget * parent,
                      Config::Class & cfg_,
                      ArticleNetworkAccessManager & articleNetMgr,                      
                      std::vector< sptr< Dictionary::Class > > const & allDictionaries_,
                      Instances::Groups const & groups_,
                      History & history_ ):
  QDialog( parent ),
  cfg( cfg_ ),
  isScanningEnabled( false ),
  allDictionaries( allDictionaries_ ),
  groups( groups_ ),
  history( history_ ),
  escapeAction( this ),
  wordFinder( this ),
  mouseEnteredOnce( false ),
  mouseIntercepted( false ),
  hideTimer( this )
{
  ui.setupUi( this );

  ui.queryError->hide();

  definition = new ArticleView( ui.outerFrame, articleNetMgr, allDictionaries,
                                groups, true, cfg );

  applyZoomFactor();
  
  ui.mainLayout->addWidget( definition );

  ui.wordListButton->hide();
  ui.pronounceButton->hide();

  ui.groupList->fill( groups );
  ui.groupList->setCurrentGroup( cfg.lastPopupGroupId );

  setWindowFlags( popupWindowFlags );

  if ( cfg.lastPopupSize.isValid() )
    resize( cfg.lastPopupSize );

  definition->focus();

  #if 0 // Experimental code to give window a non-rectangular shape (i.e.
        // balloon) using a colorkey mask.
  QPixmap pixMask( size() );
  render( &pixMask );

  setMask( pixMask.createMaskFromColor( QColor( 255, 0, 0 ) ) );

  // This helps against flickering
  setAttribute( Qt::WA_NoSystemBackground );
  #endif

  escapeAction.setShortcut( QKeySequence( "Esc" ) );
  addAction( &escapeAction );
  connect( &escapeAction, SIGNAL( triggered() ),
           this, SLOT( escapePressed() ) );

  connect( ui.groupList, SIGNAL( currentIndexChanged( QString const & ) ),
           this, SLOT( currentGroupChanged( QString const & ) ) );

  connect( &wordFinder, SIGNAL( finished() ),
           this, SLOT( prefixMatchFinished() ) );

  connect( ui.pinButton, SIGNAL( clicked( bool ) ),
           this, SLOT( pinButtonClicked( bool ) ) );

  connect( definition, SIGNAL( pageLoaded( ArticleView * ) ),
           this, SLOT( pageLoaded( ArticleView * ) ) );

  connect( QApplication::clipboard(), SIGNAL( changed( QClipboard::Mode ) ),
           this, SLOT( clipboardChanged( QClipboard::Mode ) ) );

  connect( &MouseOver::instance(), SIGNAL( hovered( QString const & ) ),
           this, SLOT( mouseHovered( QString const & ) ) );

  hideTimer.setSingleShot( true );
  hideTimer.setInterval( 400 );

  connect( &hideTimer, SIGNAL( timeout() ),
           this, SLOT( hideTimerExpired() ) );

  altModeExpirationTimer.setSingleShot( true );
  altModeExpirationTimer.setInterval( cfg.preferences.scanPopupAltModeSecs * 1000 );

  connect( &altModeExpirationTimer, SIGNAL( timeout() ),
           this, SLOT( altModeExpired() ) );

  // This one polls constantly for modifiers while alt mode lasts
  altModePollingTimer.setSingleShot( false );
  altModePollingTimer.setInterval( 50 );
  connect( &altModePollingTimer, SIGNAL( timeout() ),
           this, SLOT( altModePoll() ) );
}

ScanPopup::~ScanPopup()
{
  disableScanning();
}

void ScanPopup::enableScanning()
{
  if ( !isScanningEnabled )
  {
    isScanningEnabled = true;
    MouseOver::instance().enableMouseOver();
  }
}

void ScanPopup::disableScanning()
{
  if ( isScanningEnabled )
  {
    MouseOver::instance().disableMouseOver();
    isScanningEnabled = false;
  }
}

void ScanPopup::applyZoomFactor()
{
  definition->setZoomFactor( cfg.preferences.zoomFactor );
}

void ScanPopup::translateWordFromClipboard()
{
	return translateWordFromClipboard(QClipboard::Clipboard);
}

void ScanPopup::translateWordFromSelection()
{
	return translateWordFromClipboard(QClipboard::Selection);
}

void ScanPopup::translateWordFromClipboard(QClipboard::Mode m)
{
  printf( "translating from clipboard or selection\n" );

  QString subtype = "plain";

  QString str = QApplication::clipboard()->text( subtype, m);

  str = pendingInputWord = gd::toQString( Folding::trimWhitespaceOrPunct( gd::toWString( str ) ) );

  if ( !str.size() )
    return; // Nothing there

  // In case we had any timers engaged before, cancel them now.
  altModePollingTimer.stop();
  altModeExpirationTimer.stop();

  inputWord = str;
  engagePopup(
#ifdef Q_WS_WIN
      true // We only focus popup under Windows when activated via Ctrl+C+C
           // -- on Linux it already has an implicit focus
#endif
      );
}

void ScanPopup::clipboardChanged( QClipboard::Mode m )
{
  if ( !isScanningEnabled )
    return;
  
  printf( "clipboard changed\n" );

  QString subtype = "plain";

  handleInputWord( QApplication::clipboard()->text( subtype, m ) );
}

void ScanPopup::mouseHovered( QString const & str )
{
  handleInputWord( str );
}

void ScanPopup::handleInputWord( QString const & str )
{
  pendingInputWord = gd::toQString( Folding::trimWhitespaceOrPunct( gd::toWString( str ) ) );

  if ( !pendingInputWord.size() )
  {
    if ( cfg.preferences.scanPopupAltMode )
    {
      // In case we had any timers engaged before, cancel them now, since
      // we're not going to translate anything anymore.
      altModePollingTimer.stop();
      altModeExpirationTimer.stop();
    }
    return;
  }

  // Check key modifiers

  if ( cfg.preferences.enableScanPopupModifiers && !checkModifiersPressed( cfg.preferences.scanPopupModifiers ) )
  {
    if ( cfg.preferences.scanPopupAltMode )
    {
      altModePollingTimer.start();
      altModeExpirationTimer.start();
    }

    return;
  }

  inputWord = pendingInputWord;
  engagePopup();
}

void ScanPopup::engagePopup( bool giveFocus )
{
  /// Too large strings make window expand which is probably not what user
  /// wants
  ui.word->setText( elideInputWord() );
 
  if ( !isVisible() )
  {
    // Need to show the window

    if ( !ui.pinButton->isChecked() )
    {
      // Decide where should the window land

      QPoint currentPos = QCursor::pos();

      QRect desktop = QApplication::desktop()->screenGeometry();

      QSize windowSize = geometry().size();

      int x, y;

      /// Try the to-the-right placement
      if ( currentPos.x() + 4 + windowSize.width() <= desktop.topRight().x() )
        x = currentPos.x() + 4;
      else
      /// Try the to-the-left placement
      if ( currentPos.x() - 4 - windowSize.width() >= desktop.x() )
        x = currentPos.x() - 4 - windowSize.width();
      else
      // Center it
        x = desktop.x() + ( desktop.width() - windowSize.width() ) / 2;

      /// Try the to-the-bottom placement
      if ( currentPos.y() + 15 + windowSize.height() <= desktop.bottomLeft().y() )
        y = currentPos.y() + 15;
      else
      /// Try the to-the-top placement
      if ( currentPos.y() - 15 - windowSize.height() >= desktop.y() )
        y = currentPos.y() - 15 - windowSize.height();
      else
      // Center it
        y = desktop.y() + ( desktop.height() - windowSize.height() ) / 2;

      move( x, y );
    }

    show();

    if ( giveFocus )
    {
      activateWindow();
      raise();
    }

    if ( !ui.pinButton->isChecked() )
    {
      mouseEnteredOnce = false;
      // Need to monitor the mouse so we know when to hide the window
      interceptMouse();
    }

    // This produced some funky mouse grip-related bugs so we commented it out
    //QApplication::processEvents(); // Make window appear immediately no matter what
  }
  else
  if ( ui.pinButton->isChecked() )
  {
    // Pinned-down window isn't always on top, so we need to raise it
    show();
    activateWindow();
    raise();
  }

  initiateTranslation();
}

QString ScanPopup::elideInputWord()
{
  return inputWord.size() > 32 ? inputWord.mid( 0, 32 ) + "..." : inputWord;
}


void ScanPopup::currentGroupChanged( QString const & )
{
  if ( isVisible() )
    initiateTranslation();

  cfg.lastPopupGroupId = ui.groupList->getCurrentGroup();
}

void ScanPopup::initiateTranslation()
{
  ui.wordListButton->hide();
  ui.pronounceButton->hide();

  definition->showDefinition( inputWord, ui.groupList->getCurrentGroup() );
  wordFinder.prefixMatch( inputWord, getActiveDicts() );

  history.addItem( History::Item( ui.groupList->getCurrentGroup(),
                                  inputWord.trimmed() ) );
  history.save();
}

vector< sptr< Dictionary::Class > > const & ScanPopup::getActiveDicts()
{
  int currentGroup = ui.groupList->currentIndex();

  return
    currentGroup < 0 || currentGroup >= (int)groups.size() ? allDictionaries : 
    groups[ currentGroup ].dictionaries;
}

bool ScanPopup::eventFilter( QObject * watched, QEvent * event )
{
  if ( mouseIntercepted )
  {
    // We're only interested in our events

    if ( event->type() == QEvent::MouseMove )
    {
      QMouseEvent * mouseEvent = ( QMouseEvent * ) event;

//      printf( "Object: %s\n", watched->objectName().toUtf8().data() );

      if ( geometry().contains( mouseEvent->globalPos() ) )
      {
//        printf( "got inside\n" );

        hideTimer.stop();
        mouseEnteredOnce = true;
        uninterceptMouse();
      }
      else
      {
//        printf( "outside\n" );
        // We're in grab mode and outside the window - calculate the
        // distance from it. We might want to hide it.

        // When the mouse has entered once, we don't allow it stayng outside,
        // but we give a grace period for it to return.
        int proximity = mouseEnteredOnce ? 0 : 60;

        // Note: watched == this ensures no other child objects popping out are
        // receiving this event, meaning there's basically nothing under the
        // cursor.
        if ( watched == this &&
             !frameGeometry().adjusted( -proximity, -proximity, proximity, proximity ).
             contains( mouseEvent->globalPos() ) )
        {
          // We've way too far from the window -- hide the popup

          // If the mouse never entered the popup, hide the window instantly --
          // the user just moved the cursor further away from the window.

          if ( !mouseEnteredOnce )
            hideWindow();
          else
            hideTimer.start();
        }
      }
    }
  }

  return QDialog::eventFilter( watched, event );
}

void ScanPopup::mousePressEvent( QMouseEvent * ev )
{
  // With mouse grabs, the press can occur anywhere on the screen, which
  // might mean hiding the window.

  if ( !frameGeometry().contains( ev->globalPos() ) )
  {
    hideWindow();

    return;
  }

  startPos = ev->globalPos();
  setCursor( Qt::ClosedHandCursor );

  QDialog::mousePressEvent( ev );
}

void ScanPopup::mouseMoveEvent( QMouseEvent * event )
{
  if ( event->buttons() && cursor().shape() == Qt::ClosedHandCursor )
  {
    QPoint newPos = event->globalPos();

    QPoint delta = newPos - startPos;

    startPos = newPos;

    // Move the window

    move( pos() + delta );
  }
 
  QDialog::mouseMoveEvent( event );
}

void ScanPopup::mouseReleaseEvent( QMouseEvent * ev )
{
  unsetCursor();
  QDialog::mouseReleaseEvent( ev );
}

void ScanPopup::leaveEvent( QEvent * event )
{
  QDialog::leaveEvent( event );

  // We hide the popup when the mouse leaves it.

  // Combo-boxes seem to generate leave events for their parents when
  // unfolded, so we check coordinates as well.
  // If the dialog is pinned, we don't hide the popup.
  // If some mouse buttons are pressed, we don't hide the popup either,
  // since it indicates the move operation is underway.
  if ( !ui.pinButton->isChecked() && !geometry().contains( QCursor::pos() ) &&
       QApplication::mouseButtons() == Qt::NoButton )
  {
    hideTimer.start();
  }
}

void ScanPopup::enterEvent( QEvent * event )
{
  QDialog::enterEvent( event );

  if ( mouseEnteredOnce )
  {
    // We "enter" first time via our event filter. This seems to evade some
    // unexpected behavior under Windows.

    // If there was a countdown to hide the window, stop it.
    hideTimer.stop();
  }
}

void ScanPopup::resizeEvent( QResizeEvent * event )
{
  cfg.lastPopupSize = event->size();

  QDialog::resizeEvent( event );
}

void ScanPopup::showEvent( QShowEvent * ev )
{
  QDialog::showEvent( ev );
  
  if ( groups.size() <= 1 ) // Only the default group? Hide then.
    ui.groupList->hide();
}

void ScanPopup::prefixMatchFinished()
{
  // Check that there's a window there at all
  if ( isVisible() )
  {
    if ( wordFinder.getErrorString().size() )
    {
      ui.queryError->setToolTip( wordFinder.getErrorString() );
      ui.queryError->show();
    }
    else
      ui.queryError->hide();

    ui.wordListButton->setVisible( wordFinder.getResults().size() );
  }
}

void ScanPopup::on_wordListButton_clicked()
{
  if ( !isVisible() )
    return;

  WordFinder::SearchResults const & results = wordFinder.getResults();

  if ( results.empty() )
    return;

  QMenu menu( this );

  unsigned total = results.size() < 40 ? results.size() : 40;

  for( unsigned x = 0; x < total; ++x )
  {
    // Some items are just too large! For now skip them.

    if ( results[ x ].first.size() > 64 )
    {
      if ( total < results.size() )
        ++total;
    }
    else
      menu.addAction( results[ x ].first );
  }

  QAction * result = menu.exec( mapToGlobal( ui.wordListButton->pos() ) +
                                  QPoint( 0, ui.wordListButton->height() ) );

  if ( result )
    definition->showDefinition( result->text(), ui.groupList->getCurrentGroup() );
}

void ScanPopup::on_pronounceButton_clicked()
{
  definition->playSound();
}

void ScanPopup::pinButtonClicked( bool checked )
{
  if ( checked )
  {
    uninterceptMouse();

    setWindowFlags( Qt::Dialog );
    setWindowTitle( elideInputWord() );
    hideTimer.stop();
  }
  else
  {
    setWindowFlags( popupWindowFlags );

    mouseEnteredOnce = true;
  }

  show();
}

void ScanPopup::hideTimerExpired()
{
  if ( isVisible() )
    hideWindow();
}

void ScanPopup::altModeExpired()
{
  // The alt mode duration has expired, so there's no need to poll for modifiers
  // anymore.
  altModePollingTimer.stop();
}

void ScanPopup::altModePoll()
{
  if ( !pendingInputWord.size() )
  {
    altModePollingTimer.stop();
    altModeExpirationTimer.stop();
  }
  else
  if ( checkModifiersPressed( cfg.preferences.scanPopupModifiers ) )
  {
    altModePollingTimer.stop();
    altModeExpirationTimer.stop();

    inputWord = pendingInputWord;
    engagePopup();
  }
}

void ScanPopup::pageLoaded( ArticleView * )
{
  ui.pronounceButton->setVisible( definition->hasSound() );

  if ( cfg.preferences.pronounceOnLoadPopup )
    definition->playSound();
}

void ScanPopup::escapePressed()
{
  if ( !definition->closeSearch() )
    hideWindow();
}

void ScanPopup::hideWindow()
{
  uninterceptMouse();

  hideTimer.stop();
  unsetCursor();
  hide();
}

void ScanPopup::interceptMouse()
{
  if ( !mouseIntercepted )
  {
    grabMouse();
    qApp->installEventFilter( this );

    mouseIntercepted = true;
  }
}

void ScanPopup::uninterceptMouse()
{
  if ( mouseIntercepted )
  {
    qApp->removeEventFilter( this );
    releaseMouse();

    mouseIntercepted = false;
  }
}
