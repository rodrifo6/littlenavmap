/*****************************************************************************
* Copyright 2015-2018 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "mapgui/mapwidget.h"

#include "options/optiondata.h"
#include "navapp.h"
#include "common/constants.h"
#include "mapgui/mappaintlayer.h"
#include "settings/settings.h"
#include "common/elevationprovider.h"
#include "gui/mainwindow.h"
#include "mapgui/mapscale.h"
#include "geo/calculations.h"
#include "common/maptools.h"
#include "common/mapcolors.h"
#include "connect/connectclient.h"
#include "fs/sc/simconnectuseraircraft.h"
#include "route/route.h"
#include "userdata/userdataicons.h"
#include "route/routecontroller.h"
#include "online/onlinedatacontroller.h"
#include "atools.h"
#include "query/mapquery.h"
#include "query/airportquery.h"
#include "mapgui/maptooltip.h"
#include "common/symbolpainter.h"
#include "mapgui/mapscreenindex.h"
#include "mapgui/mapvisible.h"
#include "ui_mainwindow.h"
#include "gui/actiontextsaver.h"
#include "util/htmlbuilder.h"
#include "mapgui/maplayersettings.h"
#include "common/unit.h"
#include "gui/widgetstate.h"
#include "gui/application.h"
#include "sql/sqlrecord.h"
#include "gui/trafficpatterndialog.h"
#include "common/jumpback.h"
#include "route/routealtitude.h"

#include <QContextMenuEvent>
#include <QToolTip>
#include <QMessageBox>
#include <QPainter>

#include <marble/MarbleLocale.h>
#include <marble/MarbleWidgetInputHandler.h>
#include <marble/MarbleModel.h>
#include <marble/AbstractFloatItem.h>

// Default zoom distance if start position was not set (usually first start after installation */
const int DEFAULT_MAP_DISTANCE = 7000;

// Disable center waypoint and aircraft if distance to flight plan is larger
const float MAX_FLIGHT_PLAN_DIST_FOR_CENTER_NM = 50.f;

// Get elevation when mouse is still
const int ALTITUDE_UPDATE_TIMEOUT = 200;

// Delay recognition to avoid detection of bumps
const int TAKEOFF_LANDING_TIMEOUT = 5000;

/* If width and height of a bounding rect are smaller than this use show point */
const float POS_IS_POINT_EPSILON = 0.0001f;

// Update rates defined by delta values
const static QHash<opts::SimUpdateRate, MapWidget::SimUpdateDelta> SIM_UPDATE_DELTA_MAP(
{
  // manhattanLengthDelta; headingDelta; speedDelta; altitudeDelta; timeDeltaMs;
  {
    opts::FAST, {0.5f, 1.f, 1.f, 1.f, 75}
  },
  {
    opts::MEDIUM, {1, 1.f, 10.f, 10.f, 250}
  },
  {
    opts::LOW, {2, 4.f, 10.f, 100.f, 550}
  }
});

/* Update rate on tooltip for bearing display */
const int MAX_SIM_UPDATE_TOOLTIP_MS = 500;

const static double MINIMUM_DISTANCE = 0.1;
const static double MAXIMUM_DISTANCE = 6000.;

using namespace Marble;
using atools::gui::MapPosHistoryEntry;
using atools::gui::MapPosHistory;
using atools::geo::Rect;
using atools::geo::Pos;
using atools::sql::SqlRecord;
using atools::fs::sc::SimConnectAircraft;
using atools::fs::sc::SimConnectUserAircraft;
using atools::almostNotEqual;

MapWidget::MapWidget(MainWindow *parent)
  : Marble::MarbleWidget(parent), mainWindow(parent)
{
  mapQuery = NavApp::getMapQuery();
  airportQuery = NavApp::getAirportQuerySim();

  setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
  setMinimumSize(QSize(50, 50));

  // Event filter needed to disable some unwanted Marble default functionality
  installEventFilter(this);

  // Set the map quality to gain speed
  setMapQualityForViewContext(HighQuality, Still);
  setMapQualityForViewContext(LowQuality, Animation);

  // All nautical miles and feet for now
  MarbleGlobal::getInstance()->locale()->setMeasurementSystem(MarbleLocale::NauticalSystem);

  // Avoid stuttering movements
  inputHandler()->setInertialEarthRotationEnabled(false);

  mapTooltip = new MapTooltip(mainWindow);

  paintLayer = new MapPaintLayer(this, mapQuery);
  addLayer(paintLayer);

  screenIndex = new MapScreenIndex(this, paintLayer);

  // Disable all unwante popups on mouse click
  MarbleWidgetInputHandler *input = inputHandler();
  input->setMouseButtonPopupEnabled(Qt::RightButton, false);
  input->setMouseButtonPopupEnabled(Qt::LeftButton, false);

  screenSearchDistance = OptionData::instance().getMapClickSensitivity();
  screenSearchDistanceTooltip = OptionData::instance().getMapTooltipSensitivity();
  setSunShadingDimFactor(static_cast<double>(OptionData::instance().getDisplaySunShadingDimFactor()) / 100.);

  // "Compass" id "compass"
  // "License" id "license"
  // "Scale Bar" id "scalebar"
  // "Navigation" id "navigation"
  // "Overview Map" id "overviewmap"
  mapOverlays.insert("compass", mainWindow->getUi()->actionMapOverlayCompass);
  mapOverlays.insert("scalebar", mainWindow->getUi()->actionMapOverlayScalebar);
  mapOverlays.insert("navigation", mainWindow->getUi()->actionMapOverlayNavigation);
  mapOverlays.insert("overviewmap", mainWindow->getUi()->actionMapOverlayOverview);

  elevationDisplayTimer.setInterval(ALTITUDE_UPDATE_TIMEOUT);
  elevationDisplayTimer.setSingleShot(true);
  connect(&elevationDisplayTimer, &QTimer::timeout, this, &MapWidget::elevationDisplayTimerTimeout);

  jumpBack = new JumpBack(this);
  connect(jumpBack, &JumpBack::jumpBack, this, &MapWidget::jumpBackToAircraftTimeout);

  takeoffLandingTimer.setSingleShot(true);
  connect(&takeoffLandingTimer, &QTimer::timeout, this, &MapWidget::takeoffLandingTimeout);

  mapVisible = new MapVisible(paintLayer);
}

MapWidget::~MapWidget()
{
  elevationDisplayTimer.stop();
  takeoffLandingTimer.stop();

  qDebug() << Q_FUNC_INFO << "removeEventFilter";
  removeEventFilter(this);

  qDebug() << Q_FUNC_INFO << "delete mapVisible";
  delete mapVisible;

  qDebug() << Q_FUNC_INFO << "delete jumpBack";
  delete jumpBack;

  qDebug() << Q_FUNC_INFO << "delete paintLayer";
  delete paintLayer;

  qDebug() << Q_FUNC_INFO << "delete mapTooltip";
  delete mapTooltip;

  qDebug() << Q_FUNC_INFO << "delete screenIndex";
  delete screenIndex;
}

void MapWidget::setTheme(const QString& theme, int index)
{
  Q_UNUSED(index);
  qDebug() << "setting map theme to " << theme;

  Ui::MainWindow *ui = mainWindow->getUi();
  currentComboIndex = map::MapThemeComboIndex(index);

  // Ignore any overlay state signals the widget sends while switching theme
  ignoreOverlayUpdates = true;

  if(index >= map::CUSTOM)
  {
    // Enable all buttons for custom maps
    ui->actionMapShowCities->setEnabled(true);
    ui->actionMapShowHillshading->setEnabled(true);
    ui->actionMapShowSunShading->setEnabled(true);
  }
  else
  {
    // Update theme specific options
    switch(index)
    {
      case map::STAMENTERRAIN:
        ui->actionMapShowCities->setEnabled(false);
        ui->actionMapShowHillshading->setEnabled(false);
        ui->actionMapShowSunShading->setEnabled(true);
        break;

      case map::OPENTOPOMAP:
        setPropertyValue("ice", false);
        setShowIceLayer(false);
        ui->actionMapShowCities->setEnabled(false);
        ui->actionMapShowHillshading->setEnabled(false);
        ui->actionMapShowSunShading->setEnabled(true);
        break;

      case map::OPENSTREETMAPROADS:
      case map::OPENSTREETMAP:
      case map::CARTODARK:
      case map::CARTOLIGHT:
        ui->actionMapShowCities->setEnabled(false);
        ui->actionMapShowHillshading->setEnabled(true);
        ui->actionMapShowSunShading->setEnabled(true);
        break;

      case map::SIMPLE:
      case map::PLAIN:
      case map::ATLAS:
        ui->actionMapShowCities->setEnabled(true);
        ui->actionMapShowHillshading->setEnabled(false);
        ui->actionMapShowSunShading->setEnabled(false);
        break;
      case map::INVALID:
        qWarning() << "Invalid theme index" << index;
        break;
    }
  }

  setMapThemeId(theme);
  updateMapObjectsShown();

  ignoreOverlayUpdates = false;

  // Show or hide overlays again
  overlayStateFromMenu();
}

void MapWidget::optionsChanged()
{
  screenSearchDistance = OptionData::instance().getMapClickSensitivity();
  screenSearchDistanceTooltip = OptionData::instance().getMapTooltipSensitivity();

  // Updated sun shadow and force a tile refresh by changing the show status again
  setSunShadingDimFactor(static_cast<double>(OptionData::instance().getDisplaySunShadingDimFactor()) / 100.);
  setShowSunShading(showSunShading());

  // reloadMap();
  updateCacheSizes();
  update();
}

void MapWidget::styleChanged()
{
  update();
}

void MapWidget::updateCacheSizes()
{
  quint64 volCacheKb = OptionData::instance().getCacheSizeMemoryMb() * 1000L;
  if(volCacheKb != volatileTileCacheLimit())
  {
    qDebug() << "Volatile cache to" << volCacheKb << "kb";
    setVolatileTileCacheLimit(volCacheKb);
  }

  quint64 persCacheKb = OptionData::instance().getCacheSizeDiskMb() * 1000L;
  if(persCacheKb != model()->persistentTileCacheLimit())
  {
    qDebug() << "Persistent cache to" << persCacheKb << "kb";
    model()->setPersistentTileCacheLimit(persCacheKb);
  }
}

void MapWidget::updateMapObjectsShown()
{
  Ui::MainWindow *ui = mainWindow->getUi();

  // Sun shading ====================================================
  setShowMapSunShading(ui->actionMapShowSunShading->isChecked() &&
                       currentComboIndex != map::SIMPLE && currentComboIndex != map::PLAIN
                       && currentComboIndex != map::ATLAS);
  paintLayer->setSunShading(sunShadingFromUi());

  // Weather source ====================================================
  paintLayer->setWeatherSource(weatherSourceFromUi());

  // Other map features ====================================================
  setShowMapPois(ui->actionMapShowCities->isChecked() &&
                 (currentComboIndex == map::SIMPLE || currentComboIndex == map::PLAIN
                  || currentComboIndex == map::ATLAS));
  setShowGrid(ui->actionMapShowGrid->isChecked());

  setPropertyValue("hillshading", ui->actionMapShowHillshading->isChecked() &&
                   (currentComboIndex == map::OPENSTREETMAP ||
                    currentComboIndex == map::OPENSTREETMAPROADS ||
                    currentComboIndex == map::CARTODARK ||
                    currentComboIndex == map::CARTOLIGHT ||
                    currentComboIndex >= map::CUSTOM));

  setShowMapFeatures(map::MISSED_APPROACH, ui->actionInfoApproachShowMissedAppr->isChecked());

  setShowMapFeatures(map::AIRWAYV, ui->actionMapShowVictorAirways->isChecked());
  setShowMapFeatures(map::AIRWAYJ, ui->actionMapShowJetAirways->isChecked());

  setShowMapFeatures(map::AIRSPACE, getShownAirspaces().flags & map::AIRSPACE_ALL &&
                     ui->actionShowAirspaces->isChecked());
  setShowMapFeatures(map::AIRSPACE_ONLINE, getShownAirspaces().flags & map::AIRSPACE_ALL &&
                     ui->actionShowAirspacesOnline->isChecked());

  setShowMapFeatures(map::FLIGHTPLAN, ui->actionMapShowRoute->isChecked());
  setShowMapFeatures(map::COMPASS_ROSE, ui->actionMapShowCompassRose->isChecked());
  setShowMapFeatures(map::AIRCRAFT, ui->actionMapShowAircraft->isChecked());
  setShowMapFeatures(map::AIRCRAFT_TRACK, ui->actionMapShowAircraftTrack->isChecked());
  setShowMapFeatures(map::AIRCRAFT_AI, ui->actionMapShowAircraftAi->isChecked());
  setShowMapFeatures(map::AIRCRAFT_AI_SHIP, ui->actionMapShowAircraftAiBoat->isChecked());

  setShowMapFeatures(map::AIRPORT_HARD, ui->actionMapShowAirports->isChecked());
  setShowMapFeatures(map::AIRPORT_SOFT, ui->actionMapShowSoftAirports->isChecked());

  // Display types which are not used in structs
  setShowMapFeaturesDisplay(map::AIRPORT_WEATHER, ui->actionMapShowAirportWeather->isChecked());
  setShowMapFeaturesDisplay(map::MINIMUM_ALTITUDE, ui->actionMapShowMinimumAltitude->isChecked());

  // Force addon airport independent of other settings or not
  setShowMapFeatures(map::AIRPORT_ADDON, ui->actionMapShowAddonAirports->isChecked());

  if(OptionData::instance().getFlags() & opts::MAP_EMPTY_AIRPORTS)
  {
    // Treat empty airports special
    setShowMapFeatures(map::AIRPORT_EMPTY, ui->actionMapShowEmptyAirports->isChecked());

    // Set the general airport flag if any airport is selected
    setShowMapFeatures(map::AIRPORT,
                       ui->actionMapShowAirports->isChecked() ||
                       ui->actionMapShowSoftAirports->isChecked() ||
                       ui->actionMapShowEmptyAirports->isChecked() ||
                       ui->actionMapShowAddonAirports->isChecked());
  }
  else
  {
    // Treat empty airports as all others
    setShowMapFeatures(map::AIRPORT_EMPTY, true);

    // Set the general airport flag if any airport is selected
    setShowMapFeatures(map::AIRPORT,
                       ui->actionMapShowAirports->isChecked() ||
                       ui->actionMapShowSoftAirports->isChecked() ||
                       ui->actionMapShowAddonAirports->isChecked());
  }

  setShowMapFeatures(map::VOR, ui->actionMapShowVor->isChecked());
  setShowMapFeatures(map::NDB, ui->actionMapShowNdb->isChecked());
  setShowMapFeatures(map::ILS, ui->actionMapShowIls->isChecked());
  setShowMapFeatures(map::WAYPOINT, ui->actionMapShowWp->isChecked());

  mapVisible->updateVisibleObjectsStatusBar();

  emit shownMapFeaturesChanged(paintLayer->getShownMapObjects());

  // Update widget
  update();
}

void MapWidget::setShowMapSunShading(bool show)
{
  setShowSunShading(show);
}

void MapWidget::updateSunShadingOption()
{
  paintLayer->setSunShading(sunShadingFromUi());
}

void MapWidget::weatherUpdated()
{
  if(paintLayer->getShownMapObjects() | map::AIRPORT_WEATHER)
    update();
}

map::MapWeatherSource MapWidget::getMapWeatherSource() const
{
  return paintLayer->getWeatherSource();
}

QDateTime MapWidget::getSunShadingDateTime() const
{
  return model()->clockDateTime();
}

void MapWidget::setSunShadingDateTime(const QDateTime& datetime)
{
  if(std::abs(datetime.toSecsSinceEpoch() - model()->clockDateTime().toSecsSinceEpoch()) > 300)
  {
    // Update only if difference more than 5 minutes
    model()->setClockDateTime(datetime);
    update();
  }
}

void MapWidget::setShowMapPois(bool show)
{
  // Enable all POI stuff
  setShowPlaces(show);
  setShowCities(show);
  setShowOtherPlaces(show);
  setShowTerrain(show);
}

void MapWidget::setShowMapFeatures(map::MapObjectTypes type, bool show)
{
  paintLayer->setShowMapObjects(type, show);

  if(type & map::AIRWAYV || type & map::AIRWAYJ)
    screenIndex->updateAirwayScreenGeometry(currentViewBoundingBox);

  if(type & map::AIRSPACE || type & map::AIRSPACE_ONLINE)
    screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
}

void MapWidget::setShowMapFeaturesDisplay(map::MapObjectDisplayTypes type, bool show)
{
  paintLayer->setShowMapObjectsDisplay(type, show);
}

void MapWidget::setShowMapAirspaces(map::MapAirspaceFilter types)
{
  paintLayer->setShowAirspaces(types);
  mapVisible->updateVisibleObjectsStatusBar();
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
}

void MapWidget::setDetailLevel(int factor)
{
  qDebug() << "setDetailFactor" << factor;
  paintLayer->setDetailFactor(factor);
  mapVisible->updateVisibleObjectsStatusBar();
  screenIndex->updateAirwayScreenGeometry(currentViewBoundingBox);
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
}

map::MapObjectTypes MapWidget::getShownMapFeatures() const
{
  return paintLayer->getShownMapObjects();
}

map::MapAirspaceFilter MapWidget::getShownAirspaces() const
{
  return paintLayer->getShownAirspaces();
}

map::MapAirspaceFilter MapWidget::getShownAirspaceTypesByLayer() const
{
  return paintLayer->getShownAirspacesTypesByLayer();
}

void MapWidget::getUserpointDragPoints(QPoint& cur, QPixmap& pixmap)
{
  cur = userpointDragCur;
  pixmap = userpointDragPixmap;
}

void MapWidget::getRouteDragPoints(atools::geo::Pos& from, atools::geo::Pos& to, QPoint& cur)
{
  from = routeDragFrom;
  to = routeDragTo;
  cur = routeDragCur;
}

void MapWidget::preDatabaseLoad()
{
  jumpBackToAircraftCancel();
  cancelDragAll();
  databaseLoadStatus = true;
  paintLayer->preDatabaseLoad();
}

void MapWidget::postDatabaseLoad()
{
  databaseLoadStatus = false;
  paintLayer->postDatabaseLoad();
  screenIndex->updateAirwayScreenGeometry(currentViewBoundingBox);
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
  screenIndex->updateRouteScreenGeometry(currentViewBoundingBox);
  update();
  mapVisible->updateVisibleObjectsStatusBar();
}

void MapWidget::historyNext()
{
  const MapPosHistoryEntry& entry = history.next();
  if(entry.isValid())
  {
    jumpBackToAircraftStart(true /* save distance too */);

    // Do not fix zoom - display as is
    setDistanceToMap(entry.getDistance(), false /* Allow adjust zoom */);
    centerPosOnMap(entry.getPos());
    noStoreInHistory = true;
    mainWindow->setStatusMessage(tr("Map position history next."));
    showAircraft(false);
  }
}

void MapWidget::historyBack()
{
  const MapPosHistoryEntry& entry = history.back();
  if(entry.isValid())
  {
    jumpBackToAircraftStart(true /* save distance too */);

    // Do not fix zoom - display as is
    setDistanceToMap(entry.getDistance(), false /* Allow adjust zoom */);
    centerPosOnMap(entry.getPos());
    noStoreInHistory = true;
    mainWindow->setStatusMessage(tr("Map position history back."));
    showAircraft(false);
  }
}

void MapWidget::saveState()
{
  atools::settings::Settings& s = atools::settings::Settings::instance();

  writePluginSettings(*s.getQSettings());
  // Workaround to overviewmap storing absolute paths which will be invalid when moving program location
  s.remove("plugin_overviewmap/path_earth");
  s.remove("plugin_overviewmap/path_jupiter");
  s.remove("plugin_overviewmap/path_mars");
  s.remove("plugin_overviewmap/path_mercury");
  s.remove("plugin_overviewmap/path_moon");
  s.remove("plugin_overviewmap/path_neptune");
  s.remove("plugin_overviewmap/path_pluto");
  s.remove("plugin_overviewmap/path_saturn");
  s.remove("plugin_overviewmap/path_sky");
  s.remove("plugin_overviewmap/path_sun");
  s.remove("plugin_overviewmap/path_uranus");
  s.remove("plugin_overviewmap/path_venus");

  // Mark coordinates
  s.setValue(lnm::MAP_MARKLONX, searchMarkPos.getLonX());
  s.setValue(lnm::MAP_MARKLATY, searchMarkPos.getLatY());

  // Home coordinates and zoom
  s.setValue(lnm::MAP_HOMELONX, homePos.getLonX());
  s.setValue(lnm::MAP_HOMELATY, homePos.getLatY());
  s.setValue(lnm::MAP_HOMEDISTANCE, homeDistance);

  s.setValue(lnm::MAP_KMLFILES, kmlFilePaths);
  s.setValue(lnm::MAP_DETAILFACTOR, mapDetailLevel);
  s.setValueVar(lnm::MAP_AIRSPACES, QVariant::fromValue(paintLayer->getShownAirspaces()));

  // Sun shading settings =====================================
  s.setValue(lnm::MAP_SUN_SHADING_TIME_OPTION, paintLayer->getSunShading());

  // Weather source settings =====================================
  s.setValue(lnm::MAP_WEATHER_SOURCE, paintLayer->getWeatherSource());

  history.saveState(atools::settings::Settings::getConfigFilename(".history"));
  screenIndex->saveState();
  aircraftTrack.saveState();

  overlayStateToMenu();
  atools::gui::WidgetState state(lnm::MAP_OVERLAY_VISIBLE, false /*save visibility*/, true /*block signals*/);
  for(QAction *action : mapOverlays.values())
    state.save(action);
}

void MapWidget::sunShadingToUi(map::MapSunShading sunShading)
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  switch(sunShading)
  {
    case map::SUN_SHADING_SIMULATOR_TIME:
      ui->actionMapShowSunShadingSimulatorTime->setChecked(true);
      break;
    case map::SUN_SHADING_REAL_TIME:
      ui->actionMapShowSunShadingRealTime->setChecked(true);
      break;
    case map::SUN_SHADING_USER_TIME:
      ui->actionMapShowSunShadingUserTime->setChecked(true);
      break;
  }
}

map::MapSunShading MapWidget::sunShadingFromUi()
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  if(ui->actionMapShowSunShadingSimulatorTime->isChecked())
    return map::SUN_SHADING_SIMULATOR_TIME;
  else if(ui->actionMapShowSunShadingRealTime->isChecked())
    return map::SUN_SHADING_REAL_TIME;
  else if(ui->actionMapShowSunShadingUserTime->isChecked())
    return map::SUN_SHADING_USER_TIME;

  return map::SUN_SHADING_SIMULATOR_TIME;
}

void MapWidget::weatherSourceToUi(map::MapWeatherSource weatherSource)
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  switch(weatherSource)
  {
    case map::WEATHER_SOURCE_SIMULATOR:
      ui->actionMapShowWeatherSimulator->setChecked(true);
      break;
    case map::WEATHER_SOURCE_ACTIVE_SKY:
      ui->actionMapShowWeatherActiveSky->setChecked(true);
      break;
    case map::WEATHER_SOURCE_NOAA:
      ui->actionMapShowWeatherNoaa->setChecked(true);
      break;
    case map::WEATHER_SOURCE_VATSIM:
      ui->actionMapShowWeatherVatsim->setChecked(true);
      break;
    case map::WEATHER_SOURCE_IVAO:
      ui->actionMapShowWeatherIvao->setChecked(true);
      break;
  }
}

map::MapWeatherSource MapWidget::weatherSourceFromUi()
{
  Ui::MainWindow *ui = NavApp::getMainUi();
  if(ui->actionMapShowWeatherSimulator->isChecked())
    return map::WEATHER_SOURCE_SIMULATOR;
  else if(ui->actionMapShowWeatherActiveSky->isChecked())
    return map::WEATHER_SOURCE_ACTIVE_SKY;
  else if(ui->actionMapShowWeatherNoaa->isChecked())
    return map::WEATHER_SOURCE_NOAA;
  else if(ui->actionMapShowWeatherVatsim->isChecked())
    return map::WEATHER_SOURCE_VATSIM;
  else if(ui->actionMapShowWeatherIvao->isChecked())
    return map::WEATHER_SOURCE_IVAO;

  return map::WEATHER_SOURCE_SIMULATOR;
}

void MapWidget::resetSettingActionsToDefault()
{
  Ui::MainWindow *ui = NavApp::getMainUi();

  ui->actionMapShowAirports->blockSignals(true);
  ui->actionMapShowAirports->setChecked(true);
  ui->actionMapShowAirports->blockSignals(false);
  ui->actionMapShowSoftAirports->blockSignals(true);
  ui->actionMapShowSoftAirports->setChecked(true);
  ui->actionMapShowSoftAirports->blockSignals(false);
  ui->actionMapShowEmptyAirports->blockSignals(true);
  ui->actionMapShowEmptyAirports->setChecked(true);
  ui->actionMapShowEmptyAirports->blockSignals(false);
  ui->actionMapShowAddonAirports->blockSignals(true);
  ui->actionMapShowAddonAirports->setChecked(true);
  ui->actionMapShowAddonAirports->blockSignals(false);
  ui->actionMapShowVor->blockSignals(true);
  ui->actionMapShowVor->setChecked(true);
  ui->actionMapShowVor->blockSignals(false);
  ui->actionMapShowNdb->blockSignals(true);
  ui->actionMapShowNdb->setChecked(true);
  ui->actionMapShowNdb->blockSignals(false);
  ui->actionMapShowWp->blockSignals(true);
  ui->actionMapShowWp->setChecked(true);
  ui->actionMapShowWp->blockSignals(false);
  ui->actionMapShowIls->blockSignals(true);
  ui->actionMapShowIls->setChecked(true);
  ui->actionMapShowIls->blockSignals(false);
  ui->actionMapShowVictorAirways->blockSignals(true);
  ui->actionMapShowVictorAirways->setChecked(false);
  ui->actionMapShowVictorAirways->blockSignals(false);
  ui->actionMapShowJetAirways->blockSignals(true);
  ui->actionMapShowJetAirways->setChecked(false);
  ui->actionMapShowJetAirways->blockSignals(false);
  ui->actionShowAirspaces->blockSignals(true);
  ui->actionShowAirspaces->setChecked(true);
  ui->actionShowAirspaces->blockSignals(false);
  ui->actionShowAirspacesOnline->blockSignals(true);
  ui->actionShowAirspacesOnline->setChecked(true);
  ui->actionShowAirspacesOnline->blockSignals(false);
  ui->actionMapShowRoute->blockSignals(true);
  ui->actionMapShowRoute->setChecked(true);
  ui->actionMapShowRoute->blockSignals(false);
  ui->actionMapShowAircraft->blockSignals(true);
  ui->actionMapShowAircraft->setChecked(true);
  ui->actionMapShowAircraft->blockSignals(false);
  ui->actionMapShowCompassRose->blockSignals(true);
  ui->actionMapShowCompassRose->setChecked(false);
  ui->actionMapShowCompassRose->blockSignals(false);
  ui->actionMapAircraftCenter->blockSignals(true);
  ui->actionMapAircraftCenter->setChecked(true);
  ui->actionMapAircraftCenter->blockSignals(false);
  ui->actionMapShowAircraftAi->blockSignals(true);
  ui->actionMapShowAircraftAi->setChecked(true);
  ui->actionMapShowAircraftAi->blockSignals(false);
  ui->actionMapShowAircraftAiBoat->blockSignals(true);
  ui->actionMapShowAircraftAiBoat->setChecked(false);
  ui->actionMapShowAircraftAiBoat->blockSignals(false);
  ui->actionMapShowAircraftTrack->blockSignals(true);
  ui->actionMapShowAircraftTrack->setChecked(true);
  ui->actionMapShowAircraftTrack->blockSignals(false);
  ui->actionInfoApproachShowMissedAppr->blockSignals(true);
  ui->actionInfoApproachShowMissedAppr->setChecked(true);
  ui->actionInfoApproachShowMissedAppr->blockSignals(false);
}

void MapWidget::resetSettingsToDefault()
{
  paintLayer->setShowAirspaces({map::AIRSPACE_DEFAULT, map::AIRSPACE_FLAG_DEFAULT});
  mapDetailLevel = MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR;
  setMapDetail(mapDetailLevel);
}

void MapWidget::restoreState()
{
  qDebug() << Q_FUNC_INFO;
  atools::settings::Settings& s = atools::settings::Settings::instance();

  readPluginSettings(*s.getQSettings());

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_MAP_SETTINGS)
    mapDetailLevel = s.valueInt(lnm::MAP_DETAILFACTOR, MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR);
  else
    mapDetailLevel = MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR;
  setMapDetail(mapDetailLevel);

  // Sun shading settings ========================================
  map::MapSunShading sunShading =
    static_cast<map::MapSunShading>(s.valueInt(lnm::MAP_SUN_SHADING_TIME_OPTION, map::SUN_SHADING_SIMULATOR_TIME));
  sunShadingToUi(sunShading);
  paintLayer->setSunShading(sunShading);

  // Weather source settings ========================================
  map::MapWeatherSource weatherSource =
    static_cast<map::MapWeatherSource>(s.valueInt(lnm::MAP_WEATHER_SOURCE, map::WEATHER_SOURCE_SIMULATOR));
  weatherSourceToUi(weatherSource);
  paintLayer->setWeatherSource(weatherSource);

  if(s.contains(lnm::MAP_MARKLONX) && s.contains(lnm::MAP_MARKLATY))
    searchMarkPos = Pos(s.valueFloat(lnm::MAP_MARKLONX), s.valueFloat(lnm::MAP_MARKLATY));
  else
    searchMarkPos = Pos(0.f, 0.f);

  if(s.contains(lnm::MAP_HOMELONX) && s.contains(lnm::MAP_HOMELATY) && s.contains(lnm::MAP_HOMEDISTANCE))
  {
    homePos = Pos(s.valueFloat(lnm::MAP_HOMELONX), s.valueFloat(lnm::MAP_HOMELATY));
    homeDistance = s.valueFloat(lnm::MAP_HOMEDISTANCE);
  }
  else
  {
    // Looks like first start after installation
    homePos = Pos(0.f, 0.f);
    homeDistance = DEFAULT_MAP_DISTANCE;
  }

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_KML)
    kmlFilePaths = s.valueStrList(lnm::MAP_KMLFILES);
  screenIndex->restoreState();

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_TRAIL)
    aircraftTrack.restoreState();
  aircraftTrack.setMaxTrackEntries(OptionData::instance().getAircraftTrackMaxPoints());

  atools::gui::WidgetState state(lnm::MAP_OVERLAY_VISIBLE, false /*save visibility*/, true /*block signals*/);
  for(QAction *action : mapOverlays.values())
    state.restore(action);

  if(OptionData::instance().getFlags() & opts::STARTUP_LOAD_MAP_SETTINGS)
  {
    map::MapAirspaceFilter defaultValue = {map::AIRSPACE_DEFAULT, map::AIRSPACE_FLAG_DEFAULT};
    paintLayer->setShowAirspaces(s.valueVar(lnm::MAP_AIRSPACES,
                                            QVariant::fromValue(defaultValue)).value<map::MapAirspaceFilter>());
  }
  else
    paintLayer->setShowAirspaces({map::AIRSPACE_DEFAULT, map::AIRSPACE_FLAG_DEFAULT});

  restoreHistoryState();
}

void MapWidget::restoreHistoryState()
{
  history.restoreState(atools::settings::Settings::getConfigFilename(".history"));
}

void MapWidget::showOverlays(bool show)
{
  for(const QString& name : mapOverlays.keys())
  {
    AbstractFloatItem *overlay = floatItem(name);
    if(overlay != nullptr)
    {
      if(overlay->nameId() == "scalebar")
        continue;

      bool showConfig = mapOverlays.value(name)->isChecked();

      overlay->blockSignals(true);

      if(show && showConfig)
      {
        qDebug() << "showing float item" << overlay->name() << "id" << overlay->nameId();
        overlay->setVisible(true);
        overlay->show();
      }
      else
      {
        qDebug() << "hiding float item" << overlay->name() << "id" << overlay->nameId();
        overlay->setVisible(false);
        overlay->hide();
      }
      overlay->blockSignals(false);
    }
  }
}

void MapWidget::overlayStateToMenu()
{
  qDebug() << Q_FUNC_INFO << "ignoreOverlayUpdates" << ignoreOverlayUpdates;
  if(!ignoreOverlayUpdates)
  {
    for(const QString& name : mapOverlays.keys())
    {
      AbstractFloatItem *overlay = floatItem(name);
      if(overlay != nullptr)
      {
        QAction *menuItem = mapOverlays.value(name);
        menuItem->blockSignals(true);
        menuItem->setChecked(overlay->visible());
        menuItem->blockSignals(false);
      }
    }
  }
}

void MapWidget::overlayStateFromMenu()
{
  qDebug() << Q_FUNC_INFO << "ignoreOverlayUpdates" << ignoreOverlayUpdates;
  if(!ignoreOverlayUpdates)
  {
    for(const QString& name : mapOverlays.keys())
    {
      AbstractFloatItem *overlay = floatItem(name);
      if(overlay != nullptr)
      {
        bool show = mapOverlays.value(name)->isChecked();
        overlay->blockSignals(true);
        overlay->setVisible(show);
        if(show)
        {
          // qDebug() << "showing float item" << overlay->name() << "id" << overlay->nameId();
          setPropertyValue(overlay->nameId(), true);
          overlay->show();
        }
        else
        {
          // qDebug() << "hiding float item" << overlay->name() << "id" << overlay->nameId();
          setPropertyValue(overlay->nameId(), false);
          overlay->hide();
        }
        overlay->blockSignals(false);
      }
    }
  }
}

void MapWidget::connectOverlayMenus()
{
  for(QAction *action : mapOverlays.values())
    connect(action, &QAction::toggled, this, &MapWidget::overlayStateFromMenu);

  for(const QString& name : mapOverlays.keys())
  {
    AbstractFloatItem *overlay = floatItem(name);
    if(overlay != nullptr)
      connect(overlay, &Marble::AbstractFloatItem::visibilityChanged, this, &MapWidget::overlayStateToMenu);
  }
}

void MapWidget::mainWindowShown()
{
  qDebug() << Q_FUNC_INFO;

  // Create a copy of KML files where all missing files will be removed from the recent list
  QStringList copyKml(kmlFilePaths);
  for(const QString& kml : kmlFilePaths)
  {
    if(!loadKml(kml, false /* center */))
      copyKml.removeAll(kml);
  }

  kmlFilePaths = copyKml;

  // Set cache sizes from option data. This is done later in the startup process to avoid disk trashing.
  updateCacheSizes();

  overlayStateFromMenu();
  connectOverlayMenus();
  emit searchMarkChanged(searchMarkPos);
}

void MapWidget::showSavedPosOnStartup()
{
  qDebug() << Q_FUNC_INFO;

  active = true;

  const MapPosHistoryEntry& currentPos = history.current();

  if(OptionData::instance().getFlags() & opts::STARTUP_SHOW_ROUTE)
  {
    qDebug() << "Show Route" << NavApp::getRouteConst().getBoundingRect();
    if(!NavApp::getRouteConst().isFlightplanEmpty())
      showRect(NavApp::getRouteConst().getBoundingRect(), false /* double click */);
    else
      showHome();
  }
  else if(OptionData::instance().getFlags() & opts::STARTUP_SHOW_HOME)
    showHome();
  else if(OptionData::instance().getFlags() & opts::STARTUP_SHOW_LAST)
  {
    if(currentPos.isValid())
    {
      qDebug() << "Show Last" << currentPos;
      centerPosOnMap(currentPos.getPos());
      // Do not fix zoom - display as is
      setDistanceToMap(currentPos.getDistance(), false /* Allow adjust zoom */);
    }
    else
    {
      qDebug() << "Show 0,0" << currentPos;
      centerPosOnMap(Pos(0.f, 0.f));
      setDistanceToMap(DEFAULT_MAP_DISTANCE, true /* Allow adjust zoom */);
    }
  }
  history.activate();
}

void MapWidget::centerRectOnMap(const atools::geo::Rect& rect, bool allowAdjust)
{
  if(!rect.isPoint(POS_IS_POINT_EPSILON) &&
     rect.getWidthDegree() < 180.f &&
     rect.getHeightDegree() < 180.f &&
     rect.getWidthDegree() > POS_IS_POINT_EPSILON &&
     rect.getHeightDegree() > POS_IS_POINT_EPSILON)
  {
    // Make rectangle slightly bigger to avoid waypoints hiding at the window corners
    Rect scaled(rect);
    scaled.scale(1.075f, 1.075f);

    double north = scaled.getNorth(), south = scaled.getSouth(), east = scaled.getEast(), west = scaled.getWest();
    GeoDataLatLonBox box(north, south, east, west, GeoDataCoordinates::Degree);

    // Center rectangle first
    centerOn(box, false /* animated */);

    // Correct zoom - zoom out until all points are visible ==========================
    // Needed since Marble does zoom correctly
    qreal x, y;
    // Limit iterations to avoid endless loop
    int zoomIterations = 0;
    while((!screenCoordinates(west, north, x, y) || !screenCoordinates(east, south, x, y)) &&
          (zoomIterations < 10) && (zoom() < maximumZoom()))
    {
#ifdef DEBUG_INFORMATION
      qDebug() << Q_FUNC_INFO << "out distance NM" << atools::geo::meterToNm(distance() * 1000.);
#endif
      zoomOut();
      zoomIterations++;
    }

    // Correct zoom - zoom in until at least one point is not visible ==========================
    zoomIterations = 0;
    while(screenCoordinates(west, north, x, y) && screenCoordinates(east, south, x, y) &&
          (zoomIterations < 10) && (zoom() > minimumZoom()))
    {
#ifdef DEBUG_INFORMATION
      qDebug() << Q_FUNC_INFO << "in distance NM" << atools::geo::meterToNm(distance() * 1000.);
#endif
      zoomIn();
      zoomIterations++;
    }

    // Fix blurryness or zoom one out after correcting by zooming in
    if((allowAdjust && OptionData::instance().getFlags2() & opts::MAP_AVOID_BLURRED_MAP) || zoomIterations > 0)
      // Zoom out to next step to get a sharper map display
      zoomOut();
  }
  else
  {
    // Rect is a point or otherwise malformed
    centerPosOnMap(rect.getCenter());

    if(rect.getWidthDegree() < 180.f &&
       rect.getHeightDegree() < 180.f)
      setDistanceToMap(MINIMUM_DISTANCE, allowAdjust);
    else
      setDistanceToMap(MAXIMUM_DISTANCE, allowAdjust);
  }
}

void MapWidget::centerPosOnMap(const atools::geo::Pos& pos)
{
  if(pos.isValid())
  {
    Pos normPos = pos.normalized();
    centerOn(normPos.getLonX(), normPos.getLatY(), false /* animated */);
  }
}

void MapWidget::setDistanceToMap(double distance, bool allowAdjust)
{
  distance = std::min(std::max(distance, MINIMUM_DISTANCE / 2.), MAXIMUM_DISTANCE);

  setDistance(distance);

  if(allowAdjust && OptionData::instance().getFlags2() & opts::MAP_AVOID_BLURRED_MAP)
    // Zoom out to next step to get a sharper map display
    zoomOut();
}

void MapWidget::showPos(const atools::geo::Pos& pos, float distanceNm, bool doubleClick)
{
#if DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << pos << distanceNm << doubleClick;
#endif
  if(!pos.isValid())
  {
    qWarning() << Q_FUNC_INFO << "Invalid position";
    return;
  }

  hideTooltip();
  showAircraft(false);
  jumpBackToAircraftStart(true /* save distance too */);

  float dst = distanceNm;

  if(dst == 0.f)
    dst = atools::geo::nmToKm(Unit::rev(doubleClick ?
                                        OptionData::instance().getMapZoomShowClick() :
                                        OptionData::instance().getMapZoomShowMenu(), Unit::distNmF));

  centerPosOnMap(pos);
  if(dst < map::INVALID_DISTANCE_VALUE)
    setDistanceToMap(dst);
}

void MapWidget::showRect(const atools::geo::Rect& rect, bool doubleClick)
{
#if DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << rect << doubleClick;
#endif

  hideTooltip();
  showAircraft(false);
  jumpBackToAircraftStart(true /* save distance too */);

  float w = rect.getWidthDegree();
  float h = rect.getHeightDegree();

  if(!rect.isValid())
  {
    qWarning() << Q_FUNC_INFO << "invalid rectangle";
    return;
  }

  if(rect.isPoint(POS_IS_POINT_EPSILON))
    showPos(rect.getTopLeft(), 0.f, doubleClick);
  else
  {
    if(atools::almostEqual(w, 0.f, POS_IS_POINT_EPSILON))
      // Workaround for marble not being able to center certain lines
      // Turn rect into a square
      centerRectOnMap(Rect(rect.getWest() - h / 2, rect.getNorth(), rect.getEast() + h / 2, rect.getSouth()));
    else if(atools::almostEqual(h, 0.f, POS_IS_POINT_EPSILON))
      // Turn rect into a square
      centerRectOnMap(Rect(rect.getWest(), rect.getNorth() + w / 2, rect.getEast(), rect.getSouth() - w / 2));
    else
      // Center on rectangle
      centerRectOnMap(Rect(rect.getWest(), rect.getNorth(), rect.getEast(), rect.getSouth()));

    float dist = atools::geo::nmToKm(Unit::rev(doubleClick ?
                                               OptionData::instance().getMapZoomShowClick() :
                                               OptionData::instance().getMapZoomShowMenu(), Unit::distNmF));

    if(distance() < dist)
      setDistanceToMap(dist);
  }
}

void MapWidget::showSearchMark()
{
  qDebug() << "NavMapWidget::showMark" << searchMarkPos;

  hideTooltip();
  showAircraft(false);

  if(searchMarkPos.isValid())
  {
    jumpBackToAircraftStart(true /* save distance too */);
    centerPosOnMap(searchMarkPos);
    setDistanceToMap(atools::geo::nmToKm(Unit::rev(OptionData::instance().getMapZoomShowMenu(), Unit::distNmF)));
    mainWindow->setStatusMessage(tr("Showing distance search center."));
  }
}

void MapWidget::showAircraft(bool centerAircraftChecked)
{
  qDebug() << Q_FUNC_INFO;

  if(!(OptionData::instance().getFlags2() & opts::ROUTE_NO_FOLLOW_ON_MOVE) || centerAircraftChecked)
  {
    // Keep old behavior if jump back to aircraft is disabled

    // Adapt the menu item status if this method was not called by the action
    QAction *acAction = mainWindow->getUi()->actionMapAircraftCenter;
    if(acAction->isEnabled())
    {
      acAction->blockSignals(true);
      acAction->setChecked(centerAircraftChecked);
      acAction->blockSignals(false);
      qDebug() << "Aircraft center set to" << centerAircraftChecked;
    }

    if(centerAircraftChecked && screenIndex->getUserAircraft().isValid())
      centerPosOnMap(screenIndex->getUserAircraft().getPosition());
  }
}

void MapWidget::showHome()
{
  qDebug() << Q_FUNC_INFO << homePos;

  hideTooltip();
  jumpBackToAircraftStart(true /* save distance too */);
  showAircraft(false);
  if(!atools::almostEqual(homeDistance, 0.))
    // Only center position is valid - Do not fix zoom - display as is
    setDistanceToMap(homeDistance, false /* Allow adjust zoom */);

  if(homePos.isValid())
  {
    jumpBackToAircraftStart(true /* save distance too */);
    centerPosOnMap(homePos);
    mainWindow->setStatusMessage(tr("Showing home position."));
  }
}

void MapWidget::changeSearchMark(const atools::geo::Pos& pos)
{
  searchMarkPos = pos;

  // Will update any active distance search
  emit searchMarkChanged(searchMarkPos);
  update();
  mainWindow->setStatusMessage(tr("Distance search center position changed."));
}

void MapWidget::changeHome()
{
  homePos = Pos(centerLongitude(), centerLatitude());
  homeDistance = distance();
  update();
  mainWindow->setStatusMessage(QString(tr("Changed home position.")));
}

void MapWidget::changeRouteHighlights(const QList<int>& routeHighlight)
{
  screenIndex->setRouteHighlights(routeHighlight);
  update();
}

void MapWidget::routeChanged(bool geometryChanged)
{
  qDebug() << Q_FUNC_INFO;

  if(geometryChanged)
  {
    cancelDragAll();
    screenIndex->updateRouteScreenGeometry(currentViewBoundingBox);
    update();
  }
}

void MapWidget::routeAltitudeChanged(float altitudeFeet)
{
  Q_UNUSED(altitudeFeet);

  if(databaseLoadStatus)
    return;

  qDebug() << Q_FUNC_INFO;
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
  update();
}

bool MapWidget::isCenterLegAndAircraftActive()
{
  const Route& route = NavApp::getRouteConst();
  return OptionData::instance().getFlags2() & opts::ROUTE_AUTOZOOM && // Waypoint and aircraft center enabled
         !route.isEmpty() && // Have a route
         route.getActiveLegIndex() < map::INVALID_INDEX_VALUE && // Active leg present - special case 0 for one waypoint only
         screenIndex->getUserAircraft().isFlying() && // Aircraft in air
         route.getDistanceToFlightPlan() < MAX_FLIGHT_PLAN_DIST_FOR_CENTER_NM; // not too far away from flight plan
}

void MapWidget::simDataChanged(const atools::fs::sc::SimConnectData& simulatorData)
{
  const atools::fs::sc::SimConnectUserAircraft& aircraft = simulatorData.getUserAircraftConst();
  if(databaseLoadStatus || !aircraft.isValid())
    return;

  if(NavApp::getMainUi()->actionMapShowSunShadingSimulatorTime->isChecked())
    // Update sun shade on globe with simulator time
    setSunShadingDateTime(aircraft.getZuluTime());

  screenIndex->updateSimData(simulatorData);
  const atools::fs::sc::SimConnectUserAircraft& last = screenIndex->getLastUserAircraft();

  // Calculate travel distance since last takeoff event ===================================
  if(!takeoffLandingLastAircraft.isValid())
    // Set for the first time
    takeoffLandingLastAircraft = aircraft;
  else if(aircraft.isValid() && !aircraft.isSimReplay() && !takeoffLandingLastAircraft.isSimReplay())
  {
    // Use less accuracy for longer routes
    float epsilon = takeoffLandingDistanceNm > 20. ? Pos::POS_EPSILON_500M : Pos::POS_EPSILON_10M;

    // Check manhattan distance in degree to minimize samples
    if(takeoffLandingLastAircraft.getPosition().distanceSimpleTo(aircraft.getPosition()) > epsilon)
    {
      if(takeoffTimeMs > 0)
      {
        // Calculate averaget TAS
        qint64 currentSampleTime = aircraft.getZuluTime().toMSecsSinceEpoch();

        // Only every ten seconds since the simulator timestamps are not precise enough
        if(currentSampleTime > takeoffLastSampleTimeMs + 10000)
        {
          qint64 lastPeriod = currentSampleTime - takeoffLastSampleTimeMs;
          qint64 flightimeToCurrentPeriod = currentSampleTime - takeoffTimeMs;

          if(flightimeToCurrentPeriod > 0)
            takeoffLandingAverageTasKts = ((takeoffLandingAverageTasKts * (takeoffLastSampleTimeMs - takeoffTimeMs)) +
                                           (aircraft.getTrueAirspeedKts() * lastPeriod)) / flightimeToCurrentPeriod;
          takeoffLastSampleTimeMs = currentSampleTime;
        }
      }

      takeoffLandingDistanceNm +=
        atools::geo::meterToNm(takeoffLandingLastAircraft.getPosition().distanceMeterTo(aircraft.getPosition()));

      takeoffLandingLastAircraft = aircraft;
    }
  }

  // Check for takeoff or landing events ===================================
  if(last.isValid() && aircraft.isValid() &&
     !aircraft.isSimPaused() && !aircraft.isSimReplay() &&
     !last.isSimPaused() && !last.isSimReplay())
  {
    // start timer to emit takeoff/landing signal
    if(last.isFlying() != aircraft.isFlying())
      takeoffLandingTimer.start(TAKEOFF_LANDING_TIMEOUT);
  }

  // map::MapRunwayEnd runwayEnd;
  // map::MapAirport airport;
  // NavApp::getAirportQuerySim()->getBestRunwayEndForPosAndCourse(runwayEnd, airport,
  // aircraft.getPosition(), aircraft.getTrackDegTrue());

  // Create screen coordinates =============================
  CoordinateConverter conv(viewport());
  bool curPosVisible = false;
  QPoint curPoint = conv.wToS(aircraft.getPosition(), CoordinateConverter::DEFAULT_WTOS_SIZE, &curPosVisible);
  QPoint diff = curPoint - conv.wToS(last.getPosition());
  const OptionData& od = OptionData::instance();
  QRect widgetRect = rect();

  // Used to check if objects are still visible
  QRect widgetRectSmall = widgetRect.adjusted(10, 10, -10, -10);
  curPosVisible = widgetRectSmall.contains(curPoint);

  bool wasEmpty = aircraftTrack.isEmpty();
#ifdef DEBUG_INFORMATION_DISABLED
  qDebug() << "curPos" << curPos;
  qDebug() << "widgetRectSmall" << widgetRectSmall;
#endif

  if(aircraftTrack.appendTrackPos(aircraft.getPosition(), aircraft.getZuluTime(), aircraft.isOnGround()))
    emit aircraftTrackPruned();

  if(wasEmpty != aircraftTrack.isEmpty())
    // We have a track - update toolbar and menu
    emit updateActionStates();

  // ================================================================================
  // Update tooltip for bearing
  qint64 now = QDateTime::currentMSecsSinceEpoch();
  if(now - lastSimUpdateTooltipMs > MAX_SIM_UPDATE_TOOLTIP_MS)
  {
    lastSimUpdateTooltipMs = now;
    if((mapSearchResultTooltip.hasAirports() || mapSearchResultTooltip.hasVor() || mapSearchResultTooltip.hasNdb() ||
        mapSearchResultTooltip.hasWaypoints() || mapSearchResultTooltip.hasUserpoints()) &&
       NavApp::isConnectedAndAircraft())
    {
      updateTooltip();
    }
  }

  // ================================================================================
  // Check if screen has to be updated/scrolled/zoomed
  if(paintLayer->getShownMapObjects() & map::AIRCRAFT ||
     paintLayer->getShownMapObjects() & map::AIRCRAFT_AI ||
     paintLayer->getShownMapObjects() & map::AIRCRAFT_ONLINE)
  {
    // Show aircraft is enabled
    bool centerAircraft = mainWindow->getUi()->actionMapAircraftCenter->isChecked();

    // Get delta values for update rate
    const SimUpdateDelta& deltas = SIM_UPDATE_DELTA_MAP.value(od.getSimUpdateRate());

    // Limit number of updates per second =================================================
    if(now - lastSimUpdateMs > deltas.timeDeltaMs)
    {
      lastSimUpdateMs = now;

      // Check if any AI aircraft are visible
      bool aiVisible = false;
      if(paintLayer->getShownMapObjects() & map::AIRCRAFT_AI ||
         paintLayer->getShownMapObjects() & map::AIRCRAFT_AI_SHIP ||
         paintLayer->getShownMapObjects() & map::AIRCRAFT_ONLINE)
      {
        for(const atools::fs::sc::SimConnectAircraft& ai : simulatorData.getAiAircraftConst())
        {
          if(currentViewBoundingBox.contains(
               Marble::GeoDataCoordinates(ai.getPosition().getLonX(), ai.getPosition().getLatY(), 0,
                                          Marble::GeoDataCoordinates::Degree)))
          {
            aiVisible = true;
            break;
          }
        }
      }

      // Check if position has changed significantly
      bool posHasChanged = !last.isValid() || // No previous position
                           diff.manhattanLength() >= deltas.manhattanLengthDelta; // Screen position has changed

      // Check if any data like heading has changed which requires a redraw
      bool dataHasChanged = posHasChanged ||
                            almostNotEqual(last.getHeadingDegMag(), aircraft.getHeadingDegMag(), deltas.headingDelta) || // Heading has changed
                            almostNotEqual(last.getIndicatedSpeedKts(),
                                           aircraft.getIndicatedSpeedKts(), deltas.speedDelta) || // Speed has changed
                            almostNotEqual(last.getPosition().getAltitude(),
                                           aircraft.getPosition().getAltitude(), deltas.altitudeDelta); // Altitude has changed

      if(dataHasChanged)
        screenIndex->updateLastSimData(simulatorData);

      // Option to udpate always
      bool updateAlways = od.getFlags() & opts::SIM_UPDATE_MAP_CONSTANTLY;

      // Check if centering of leg is reqired =======================================
      const Route& route = NavApp::getRouteConst();
      const RouteLeg *activeLeg = route.getActiveLeg();
      bool centerAircraftAndLeg = isCenterLegAndAircraftActive();

      // Get position of next waypoint and check visibility
      Pos nextWpPos;
      QPoint nextWpPoint;
      bool nextWpPosVisible = false;
      if(centerAircraftAndLeg)
      {
        nextWpPos = activeLeg != nullptr ? route.getActiveLeg()->getPosition() : Pos();
        nextWpPoint = conv.wToS(nextWpPos, CoordinateConverter::DEFAULT_WTOS_SIZE, &nextWpPosVisible);
        nextWpPosVisible = widgetRectSmall.contains(nextWpPoint);
      }

      if(centerAircraft && !contextMenuActive) // centering required by button but not while menu is open
      {
        if(!curPosVisible || // Not visible on world map
           posHasChanged) // Significant change in position might require zooming or re-centering
        {
          // Do not update if user is using drag and drop or scrolling around
          // No updates while jump back is active and user is moving around
          if(mouseState == mw::NONE && viewContext() == Marble::Still && !jumpBack->isActive())
          {
            if(centerAircraftAndLeg)
            {
              // Update four times based on flying time to next waypoint - this is recursive
              // and will update more often close to the wp
              int timeToWpUpdateMs =
                std::max(static_cast<int>(atools::geo::meterToNm(aircraft.getPosition().distanceMeterTo(nextWpPos)) /
                                          (aircraft.getGroundSpeedKts() + 1.f) * 3600.f / 4.f), 4) * 1000;

              // Zoom to rectangle every 15 seconds
              bool zoomToRect = now - lastCenterAcAndWp > timeToWpUpdateMs;

#ifdef DEBUG_INFORMATION_SIMUPDATE
              qDebug() << Q_FUNC_INFO;
              qDebug() << "curPosVisible" << curPosVisible;
              qDebug() << "nextWpPosVisible" << nextWpPosVisible;
              qDebug() << "updateAlways" << updateAlways;
              qDebug() << "zoomToRect" << zoomToRect;
#endif
              if(!curPosVisible || !nextWpPosVisible || updateAlways || zoomToRect)
              {
                // Wait 15 seconds after every update
                lastCenterAcAndWp = now;

                // Postpone screen updates
                setUpdatesEnabled(false);

                Rect rect(nextWpPos);
                rect.extend(aircraft.getPosition());

                if(rect.getWidthDegree() > 180.f || rect.getHeightDegree() > 180.f)
                  rect = Rect(nextWpPos);

#ifdef DEBUG_INFORMATION_SIMUPDATE
                qDebug() << Q_FUNC_INFO;
                qDebug() << "curPoint" << curPoint;
                qDebug() << "nextWpPoint" << nextWpPoint;
                qDebug() << "widgetRect" << widgetRect;
                qDebug() << "ac.getPosition()" << aircraft.getPosition();
                qDebug() << "rect" << rect;
#endif

                if(!rect.isPoint(POS_IS_POINT_EPSILON))
                {
                  centerRectOnMap(rect);

                  float altToZoom = aircraft.getAltitudeAboveGroundFt() > 12000.f ? 1400.f : 2800.f;
                  // Minimum zoom depends on flight altitude
                  float minZoomDist = atools::geo::nmToKm(
                    std::min(std::max(aircraft.getAltitudeAboveGroundFt() / altToZoom, 0.4f), 28.f));

                  if(distance() < minZoomDist)
                  {
#ifdef DEBUG_INFORMATION
                    qDebug() << Q_FUNC_INFO << "distance() < minZoom" << distance() << "<" << minZoomDist;
#endif
                    // Correct zoom for minimum distance
                    setDistanceToMap(minZoomDist);
#ifdef DEBUG_INFORMATION
                    qDebug() << Q_FUNC_INFO << "zoom()" << zoom();
#endif
                  }
                }
                else if(rect.isValid())
                  centerPosOnMap(aircraft.getPosition());
              } // if(!curPosVisible || !nextWpPosVisible || updateAlways || rectTooSmall || !centered)
            } // if(centerAircraftAndLeg)
            else
            {
              // Calculate the amount that has to be substracted from each side of the rectangle
              float boxFactor = (100.f - od.getSimUpdateBox()) / 100.f / 2.f;
              int dx = static_cast<int>(width() * boxFactor);
              int dy = static_cast<int>(height() * boxFactor);

              widgetRect.adjust(dx, dy, -dx, -dy);

              if(!widgetRect.contains(curPoint) || // Aircraft out of box or ...
                 updateAlways) // ... update always
              {
                setUpdatesEnabled(false);

                // Center aircraft only
                centerPosOnMap(aircraft.getPosition());
              }
            }
          }
        }
      }

      if(!updatesEnabled())
        setUpdatesEnabled(true);

      if((dataHasChanged || aiVisible) && !contextMenuActive)
        // Not scrolled or zoomed but needs a redraw
        update();
    }
  }
  else if(paintLayer->getShownMapObjects() & map::AIRCRAFT_TRACK)
  {
    // No aircraft but track - update track only
    if(!last.isValid() || diff.manhattanLength() > 4)
    {
      screenIndex->updateLastSimData(simulatorData);

      if(!contextMenuActive)
        update();
    }
  }
}

void MapWidget::highlightProfilePoint(const atools::geo::Pos& pos)
{
  changeProfileHighlight(pos);
}

void MapWidget::connectedToSimulator()
{
  qDebug() << Q_FUNC_INFO;
  jumpBackToAircraftCancel();
  update();
}

void MapWidget::disconnectedFromSimulator()
{
  qDebug() << Q_FUNC_INFO;
  // Clear all data on disconnect
  screenIndex->updateSimData(atools::fs::sc::SimConnectData());
  mapVisible->updateVisibleObjectsStatusBar();
  jumpBackToAircraftCancel();
  update();
}

bool MapWidget::addKmlFile(const QString& kmlFile)
{
  if(loadKml(kmlFile, true))
  {
    // Add to the list of files that will be reloaded on startup
    kmlFilePaths.append(kmlFile);
    // Successfully loaded
    return true;
  }
  else
    // Loading failed
    return false;
}

void MapWidget::clearKmlFiles()
{
  for(const QString& file : kmlFilePaths)
    model()->removeGeoData(file);
  kmlFilePaths.clear();
}

const atools::geo::Pos& MapWidget::getProfileHighlight() const
{
  return screenIndex->getProfileHighlight();
}

void MapWidget::clearSearchHighlights()
{
  screenIndex->getSearchHighlights() = map::MapSearchResult();
  update();
}

void MapWidget::clearAirspaceHighlights()
{
  screenIndex->getAirspaceHighlights().clear();
  update();
}

bool MapWidget::hasHighlights() const
{
  return !screenIndex->getSearchHighlights().isEmpty() || !screenIndex->getAirspaceHighlights().isEmpty();
}

const map::MapSearchResult& MapWidget::getSearchHighlights() const
{
  return screenIndex->getSearchHighlights();
}

const QList<map::MapAirspace>& MapWidget::getAirspaceHighlights() const
{
  return screenIndex->getAirspaceHighlights();
}

const proc::MapProcedureLeg& MapWidget::getProcedureLegHighlights() const
{
  return screenIndex->getApproachLegHighlights();
}

const proc::MapProcedureLegs& MapWidget::getProcedureHighlight() const
{
  return screenIndex->getProcedureHighlight();
}

void MapWidget::changeApproachHighlight(const proc::MapProcedureLegs& approach)
{
#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << approach;
#endif

  cancelDragAll();
  screenIndex->getProcedureHighlight() = approach;
  screenIndex->updateRouteScreenGeometry(currentViewBoundingBox);
  update();
}

/* Also clicked airspaces in the info window */
void MapWidget::changeAirspaceHighlights(const QList<map::MapAirspace>& airspaces)
{
  screenIndex->getAirspaceHighlights() = airspaces;
  update();
}

void MapWidget::changeSearchHighlights(const map::MapSearchResult& newHighlights)
{
  screenIndex->getSearchHighlights() = newHighlights;
  update();
}

void MapWidget::changeProcedureLegHighlights(const proc::MapProcedureLeg *leg)
{
  screenIndex->setApproachLegHighlights(leg);
  update();
}

void MapWidget::changeProfileHighlight(const atools::geo::Pos& pos)
{
  if(pos != screenIndex->getProfileHighlight())
  {
    screenIndex->setProfileHighlight(pos);
    update();
  }
}

/* Update the flight plan from a drag and drop result. Show a menu if multiple objects are
 * found at the button release position. */
void MapWidget::updateRoute(QPoint newPoint, int leg, int point, bool fromClickAdd, bool fromClickAppend)
{
  qDebug() << "End route drag" << newPoint << "leg" << leg << "point" << point;

  // Get all objects where the mouse button was released
  map::MapSearchResult result;
  screenIndex->getAllNearest(newPoint.x(), newPoint.y(), screenSearchDistance, result);

  // Count number of all objects
  int totalSize = result.getTotalSize(map::AIRPORT_ALL | map::VOR | map::NDB | map::WAYPOINT | map::USERPOINT);

  int id = -1;
  map::MapObjectTypes type = map::NONE;
  if(totalSize == 0)
  {
    // Nothing at the position - add userpoint
    qDebug() << Q_FUNC_INFO << "userpoint";
    type = map::USERPOINTROUTE;
  }
  else if(totalSize == 1)
  {
    // Only one entry at the position - add single navaid without menu
    qDebug() << Q_FUNC_INFO << "navaid";
    result.getIdAndType(id, type, {map::AIRPORT, map::VOR, map::NDB, map::WAYPOINT, map::USERPOINT});
  }
  else
  {
    qDebug() << Q_FUNC_INFO << "menu";

    // Avoid drag cancel when loosing focus
    mouseState |= mw::DRAG_POST_MENU;

    QString menuText = tr("Add %1 to Flight Plan");
    if(fromClickAdd)
      menuText = tr("Insert %1 to Flight Plan");
    else if(fromClickAppend)
      menuText = tr("Append %1 to Flight Plan");

    // Multiple entries - build a menu with icons
    showFeatureSelectionMenu(id, type, result, menuText);

    mouseState &= ~mw::DRAG_POST_MENU;
  }

  Pos pos = atools::geo::EMPTY_POS;
  if(type == map::USERPOINTROUTE)
    // Get position for new user point from from screen
    pos = CoordinateConverter(viewport()).sToW(newPoint.x(), newPoint.y());

  if((id != -1 && type != map::NONE) || type == map::USERPOINTROUTE)
  {
    if(fromClickAdd)
      emit routeAdd(id, pos, type, -1 /* leg index */);
    else if(fromClickAppend)
      emit routeAdd(id, pos, type, map::INVALID_INDEX_VALUE);
    else
    {
      // From drag
      if(leg != -1)
        emit routeAdd(id, pos, type, leg);
      else if(point != -1)
        emit routeReplace(id, pos, type, point);
    }
  }
}

bool MapWidget::showFeatureSelectionMenu(int& id, map::MapObjectTypes& type, const map::MapSearchResult& result,
                                         const QString& menuText)
{
  // Add id and type to actions
  const int ICON_SIZE = 20;
  QMenu menu;
  SymbolPainter symbolPainter;

  for(const map::MapAirport& obj : result.airports)
  {
    QAction *action = new QAction(symbolPainter.createAirportIcon(obj, ICON_SIZE),
                                  menuText.arg(map::airportText(obj)), this);
    action->setData(QVariantList({obj.id, map::AIRPORT}));
    menu.addAction(action);
  }

  if(!result.airports.isEmpty() || !result.vors.isEmpty() || !result.ndbs.isEmpty() ||
     !result.waypoints.isEmpty() || !result.userpoints.isEmpty())
    // There will be more entries - add a separator
    menu.addSeparator();

  for(const map::MapVor& obj : result.vors)
  {
    QAction *action = new QAction(symbolPainter.createVorIcon(obj, ICON_SIZE),
                                  menuText.arg(map::vorText(obj)), this);
    action->setData(QVariantList({obj.id, map::VOR}));
    menu.addAction(action);
  }
  for(const map::MapNdb& obj : result.ndbs)
  {
    QAction *action = new QAction(symbolPainter.createNdbIcon(ICON_SIZE),
                                  menuText.arg(map::ndbText(obj)), this);
    action->setData(QVariantList({obj.id, map::NDB}));
    menu.addAction(action);
  }
  for(const map::MapWaypoint& obj : result.waypoints)
  {
    QAction *action = new QAction(symbolPainter.createWaypointIcon(ICON_SIZE),
                                  menuText.arg(map::waypointText(obj)), this);
    action->setData(QVariantList({obj.id, map::WAYPOINT}));
    menu.addAction(action);
  }

  int numUserpoints = 0;
  for(const map::MapUserpoint& obj : result.userpoints)
  {
    QAction *action = nullptr;
    if(numUserpoints > 5)
    {
      action = new QAction(symbolPainter.createUserpointIcon(ICON_SIZE), tr("More ..."), this);
      action->setDisabled(true);
      menu.addAction(action);
      break;
    }
    else
    {
      action = new QAction(symbolPainter.createUserpointIcon(ICON_SIZE),
                           menuText.arg(map::userpointText(obj)), this);
      action->setData(QVariantList({obj.id, map::USERPOINT}));
      menu.addAction(action);
    }
    numUserpoints++;
  }

  // Always present - userpoint
  menu.addSeparator();
  {
    QAction *action = new QAction(symbolPainter.createUserpointIcon(ICON_SIZE),
                                  menuText.arg(tr("Position")), this);
    action->setData(QVariantList({-1, map::USERPOINTROUTE}));
    menu.addAction(action);
  }

  // Always present - cancel
  menu.addSeparator();
  menu.addAction(new QAction(QIcon(":/littlenavmap/resources/icons/cancel.svg"),
                             tr("Cancel"), this));

  // Execute the menu
  QAction *action = menu.exec(QCursor::pos());

  if(action != nullptr && !action->data().isNull())
  {
    // Get id and type from selected action
    QVariantList data = action->data().toList();
    id = data.first().toInt();
    type = map::MapObjectTypes(data.at(1).toInt());
    return true;
  }
  return false;
}

void MapWidget::contextMenuEvent(QContextMenuEvent *event)
{
  qDebug() << Q_FUNC_INFO
           << "state" << mouseState
           << "modifiers" << event->modifiers()
           << "reason" << event->reason()
           << "pos" << event->pos();

  if(mouseState != mw::NONE)
    return;

  // Disable any automatic scrolling
  contextMenuActive = true;

  QPoint point;
  if(event->reason() == QContextMenuEvent::Keyboard)
    // Event does not contain position if triggered by keyboard
    point = mapFromGlobal(QCursor::pos());
  else
    point = event->pos();

  QPoint menuPos = QCursor::pos();
  // Do not show context menu if point is not on the map
  if(!rect().contains(point))
  {
    menuPos = mapToGlobal(rect().center());
    point = QPoint();
  }

  hideTooltip();

  Ui::MainWindow *ui = mainWindow->getUi();

  // ===================================================================================
  // Texts with % will be replaced save them and let the ActionTextSaver restore them on return
  atools::gui::ActionTextSaver textSaver({ui->actionMapMeasureDistance, ui->actionMapMeasureRhumbDistance,
                                          ui->actionMapRangeRings, ui->actionMapNavaidRange,
                                          ui->actionShowInSearch, ui->actionRouteAddPos, ui->actionRouteAppendPos,
                                          ui->actionMapShowInformation, ui->actionMapShowApproaches,
                                          ui->actionRouteDeleteWaypoint, ui->actionRouteAirportStart,
                                          ui->actionRouteAirportDest,
                                          ui->actionMapEditUserWaypoint, ui->actionMapUserdataAdd,
                                          ui->actionMapUserdataEdit, ui->actionMapUserdataDelete,
                                          ui->actionMapUserdataMove, ui->actionMapTrafficPattern});
  Q_UNUSED(textSaver);

  // ===================================================================================
  // Build menu - add actions
  QMenu menu;
  menu.addAction(ui->actionMapShowInformation);
  menu.addAction(ui->actionMapShowApproaches);
  menu.addSeparator();

  menu.addAction(ui->actionMapMeasureDistance);
  menu.addAction(ui->actionMapMeasureRhumbDistance);
  menu.addAction(ui->actionMapHideDistanceMarker);
  menu.addSeparator();

  menu.addAction(ui->actionMapTrafficPattern);
  menu.addAction(ui->actionMapHideTrafficPattern);
  menu.addSeparator();

  menu.addAction(ui->actionMapRangeRings);
  menu.addAction(ui->actionMapNavaidRange);
  menu.addAction(ui->actionMapHideOneRangeRing);
  menu.addSeparator();

  menu.addAction(ui->actionRouteAirportStart);
  menu.addAction(ui->actionRouteAirportDest);
  menu.addSeparator();

  menu.addAction(ui->actionRouteAddPos);
  menu.addAction(ui->actionRouteAppendPos);
  menu.addAction(ui->actionRouteDeleteWaypoint);
  menu.addAction(ui->actionMapEditUserWaypoint);
  menu.addSeparator();

  menu.addAction(ui->actionMapUserdataAdd);
  menu.addAction(ui->actionMapUserdataEdit);
  menu.addAction(ui->actionMapUserdataMove);
  menu.addAction(ui->actionMapUserdataDelete);
  menu.addSeparator();

  menu.addAction(ui->actionShowInSearch);
  menu.addSeparator();

  menu.addAction(ui->actionMapSetMark);
  menu.addAction(ui->actionMapSetHome);

  int distMarkerIndex = -1;
  int trafficPatternIndex = -1;
  int rangeMarkerIndex = -1;
  bool visibleOnMap = false;
  Pos pos;

  if(!point.isNull())
  {
    qreal lon, lat;
    // Cursor can be outside or map region
    visibleOnMap = geoCoordinates(point.x(), point.y(), lon, lat);

    if(visibleOnMap)
    {
      pos = Pos(lon, lat);
      distMarkerIndex = screenIndex->getNearestDistanceMarkIndex(point.x(), point.y(), screenSearchDistance);
      rangeMarkerIndex = screenIndex->getNearestRangeMarkIndex(point.x(), point.y(), screenSearchDistance);
      trafficPatternIndex = screenIndex->getNearestTrafficPatternIndex(point.x(), point.y(), screenSearchDistance);
    }
  }

  // Disable all menu items that depend on position
  ui->actionMapSetMark->setEnabled(visibleOnMap);
  ui->actionMapSetHome->setEnabled(visibleOnMap);
  ui->actionMapMeasureDistance->setEnabled(visibleOnMap);
  ui->actionMapMeasureRhumbDistance->setEnabled(visibleOnMap);
  ui->actionMapRangeRings->setEnabled(visibleOnMap);

  ui->actionMapUserdataAdd->setEnabled(visibleOnMap);
  ui->actionMapUserdataEdit->setEnabled(false);
  ui->actionMapUserdataDelete->setEnabled(false);
  ui->actionMapUserdataMove->setEnabled(false);

  ui->actionMapHideOneRangeRing->setEnabled(visibleOnMap && rangeMarkerIndex != -1);
  ui->actionMapHideDistanceMarker->setEnabled(visibleOnMap && distMarkerIndex != -1);
  ui->actionMapHideTrafficPattern->setEnabled(visibleOnMap && trafficPatternIndex != -1);

  ui->actionMapShowInformation->setEnabled(false);
  ui->actionMapShowApproaches->setEnabled(false);
  ui->actionMapTrafficPattern->setEnabled(false);
  ui->actionMapNavaidRange->setEnabled(false);
  ui->actionShowInSearch->setEnabled(false);
  ui->actionRouteAddPos->setEnabled(visibleOnMap);
  ui->actionRouteAppendPos->setEnabled(visibleOnMap);
  ui->actionRouteAirportStart->setEnabled(false);
  ui->actionRouteAirportDest->setEnabled(false);
  ui->actionRouteDeleteWaypoint->setEnabled(false);

  ui->actionMapEditUserWaypoint->setEnabled(false);

  // Get objects near position =============================================================
  map::MapSearchResult result;
  screenIndex->getAllNearest(point.x(), point.y(), screenSearchDistance, result);

  map::MapAirport *airport = nullptr;
  SimConnectAircraft *aiAircraft = nullptr;
  SimConnectAircraft *onlineAircraft = nullptr;
  SimConnectUserAircraft *userAircraft = nullptr;
  map::MapVor *vor = nullptr;
  map::MapNdb *ndb = nullptr;
  map::MapWaypoint *waypoint = nullptr;
  map::MapUserpointRoute *userpointRoute = nullptr;
  map::MapAirway *airway = nullptr;
  map::MapParking *parking = nullptr;
  map::MapHelipad *helipad = nullptr;
  map::MapAirspace *airspace = nullptr, *onlineCenter = nullptr;
  map::MapUserpoint *userpoint = nullptr;

  bool airportDestination = false, airportDeparture = false, routeVisible = getShownMapFeatures() & map::FLIGHTPLAN;
  // ===================================================================================
  // Get only one object of each type
  if(result.userAircraft.isValid())
    userAircraft = &result.userAircraft;

  if(!result.aiAircraft.isEmpty())
    aiAircraft = &result.aiAircraft.first();

  if(!result.onlineAircraft.isEmpty())
    onlineAircraft = &result.onlineAircraft.first();

  // Add shadow for "show in search"
  SimConnectAircraft shadowAircraft;
  if(userAircraft != nullptr && NavApp::getOnlinedataController()->getShadowAircraft(shadowAircraft, *userAircraft))
    onlineAircraft = &shadowAircraft;

  if(!result.airports.isEmpty())
  {
    airport = &result.airports.first();
    airportDestination = NavApp::getRouteConst().isAirportDestination(airport->ident);
    airportDeparture = NavApp::getRouteConst().isAirportDeparture(airport->ident);
  }

  if(!result.parkings.isEmpty())
    parking = &result.parkings.first();

  if(!result.helipads.isEmpty() && result.helipads.first().startId != -1)
    // Only helipads with start position are allowed
    helipad = &result.helipads.first();

  if(!result.vors.isEmpty())
    vor = &result.vors.first();

  if(!result.ndbs.isEmpty())
    ndb = &result.ndbs.first();

  if(!result.waypoints.isEmpty())
    waypoint = &result.waypoints.first();

  if(!result.userPointsRoute.isEmpty())
    userpointRoute = &result.userPointsRoute.first();

  if(!result.userpoints.isEmpty())
    userpoint = &result.userpoints.first();

  if(!result.airways.isEmpty())
    airway = &result.airways.first();

  if(!result.airspaces.isEmpty())
    airspace = &result.airspaces.first();

  // ===================================================================================
  // Collect information from the search result - build text only for one object for several menu items
  bool isAircraft = false;
  QString informationText, procedureText, measureText, rangeRingText, departureText, departureParkingText,
          destinationText,
          addRouteText, searchText, editUserpointText;

  if(airspace != nullptr)
  {
    if(airspace->online)
    {
      onlineCenter = airspace;
      searchText = informationText = tr("Online Center %1").arg(onlineCenter->name);
    }
    else
      informationText = tr("Airspace");
  }

  // Fill texts in reverse order of priority
  if(airway != nullptr)
    informationText = map::airwayText(*airway);

  if(userpointRoute != nullptr)
    // No show information on user point
    informationText.clear();

  if(waypoint != nullptr)
    informationText = measureText = addRouteText = searchText = map::waypointText(*waypoint);

  if(ndb != nullptr)
    informationText = measureText = rangeRingText = addRouteText = searchText = map::ndbText(*ndb);

  if(vor != nullptr)
    informationText = measureText = rangeRingText = addRouteText = searchText = map::vorText(*vor);

  if(airport != nullptr)
    procedureText = informationText = measureText = departureText
                                                      = destinationText = addRouteText =
                                                                            searchText = map::airportText(*airport);

  // Userpoints are drawn on top of all features
  if(userpoint != nullptr)
    editUserpointText = informationText = addRouteText = searchText = map::userpointText(*userpoint);

  // Override airport if part of route and visible
  if((airportDeparture || airportDestination) && airport != nullptr && routeVisible)
    informationText = addRouteText = map::airportText(*airport);

  int departureParkingAirportId = -1;
  // Parking or helipad only if no airport at cursor
  if(airport == nullptr)
  {
    if(helipad != nullptr)
    {
      departureParkingAirportId = helipad->airportId;
      departureParkingText = tr("Helipad %1").arg(helipad->runwayName);
    }

    if(parking != nullptr)
    {
      departureParkingAirportId = parking->airportId;
      if(parking->number == -1)
        departureParkingText = map::parkingName(parking->name);
      else
        departureParkingText = map::parkingName(parking->name) + " " + QLocale().toString(parking->number);
    }
  }

  if(departureParkingAirportId != -1)
  {
    // Clear texts which are not valid for parking positions
    informationText.clear();
    procedureText.clear();
    measureText.clear();
    rangeRingText.clear();
    destinationText.clear();
    addRouteText.clear();
    searchText.clear();
  }

  if(aiAircraft != nullptr)
  {
    QStringList info;
    if(!aiAircraft->getAirplaneRegistration().isEmpty())
      info.append(aiAircraft->getAirplaneRegistration());
    if(!aiAircraft->getAirplaneModel().isEmpty())
      info.append(aiAircraft->getAirplaneModel());

    if(info.isEmpty())
      // X-Plane does not give any useful information at all
      info.append(tr("AI / Multiplayer") + tr(" %1").arg(aiAircraft->getObjectId() + 1));

    informationText = info.join(tr(" / "));
    isAircraft = true;
  }

  if(onlineAircraft != nullptr)
  {
    searchText = informationText = tr("Online Client Aircraft %1").arg(onlineAircraft->getAirplaneRegistration());
    isAircraft = true;
  }

  if(userAircraft != nullptr)
  {
    informationText = tr("User Aircraft");
    isAircraft = true;
  }

  // ===================================================================================
  // Build "delete from flight plan" text
  int routeIndex = -1;
  map::MapObjectTypes deleteType = map::NONE;
  QString routeText;
  if(airport != nullptr && airport->routeIndex != -1)
  {
    routeText = map::airportText(*airport);
    routeIndex = airport->routeIndex;
    deleteType = map::AIRPORT;
  }
  else if(vor != nullptr && vor->routeIndex != -1)
  {
    routeText = map::vorText(*vor);
    routeIndex = vor->routeIndex;
    deleteType = map::VOR;
  }
  else if(ndb != nullptr && ndb->routeIndex != -1)
  {
    routeText = map::ndbText(*ndb);
    routeIndex = ndb->routeIndex;
    deleteType = map::NDB;
  }
  else if(waypoint != nullptr && waypoint->routeIndex != -1)
  {
    routeText = map::waypointText(*waypoint);
    routeIndex = waypoint->routeIndex;
    deleteType = map::WAYPOINT;
  }
  else if(userpointRoute != nullptr && userpointRoute->routeIndex != -1)
  {
    routeText = map::userpointRouteText(*userpointRoute);
    routeIndex = userpointRoute->routeIndex;
    deleteType = map::USERPOINTROUTE;
  }

  // ===================================================================================
  // Update "set airport as start/dest"
  if(airport != nullptr || departureParkingAirportId != -1)
  {
    QString airportText(departureText);

    if(departureParkingAirportId != -1)
    {
      // Get airport for parking
      map::MapAirport parkAp;
      airportQuery->getAirportById(parkAp, departureParkingAirportId);
      airportText = map::airportText(parkAp) + " / ";
    }

    ui->actionRouteAirportStart->setEnabled(true);
    ui->actionRouteAirportStart->setText(ui->actionRouteAirportStart->text().arg(airportText + departureParkingText));

    if(airport != nullptr)
    {
      ui->actionRouteAirportDest->setEnabled(true);
      ui->actionRouteAirportDest->setText(ui->actionRouteAirportDest->text().arg(destinationText));
    }
    else
      ui->actionRouteAirportDest->setText(ui->actionRouteAirportDest->text().arg(QString()));
  }
  else
  {
    // No airport or selected parking position
    ui->actionRouteAirportStart->setText(ui->actionRouteAirportStart->text().arg(QString()));
    ui->actionRouteAirportDest->setText(ui->actionRouteAirportDest->text().arg(QString()));
  }

  // ===================================================================================
  // Update "show information" for airports, navaids, airways and airspaces
  if(vor != nullptr || ndb != nullptr || waypoint != nullptr || airport != nullptr ||
     airway != nullptr || airspace != nullptr || userpoint != nullptr)
  {
    ui->actionMapShowInformation->setEnabled(true);
    ui->actionMapShowInformation->setText(ui->actionMapShowInformation->text().arg(informationText));
  }
  else
  {
    if(isAircraft)
    {
      ui->actionMapShowInformation->setEnabled(true);
      ui->actionMapShowInformation->setText(ui->actionMapShowInformation->text().arg(informationText));
    }
    else
      ui->actionMapShowInformation->setText(ui->actionMapShowInformation->text().arg(QString()));
  }

  // ===================================================================================
  // Update "edit userpoint" and "add userpoint"
  if(vor != nullptr || ndb != nullptr || waypoint != nullptr || airport != nullptr)
    ui->actionMapUserdataAdd->setText(ui->actionMapUserdataAdd->text().arg(informationText));
  else
    ui->actionMapUserdataAdd->setText(ui->actionMapUserdataAdd->text().arg(QString()));

  if(userpoint != nullptr)
  {
    ui->actionMapUserdataEdit->setEnabled(true);
    ui->actionMapUserdataEdit->setText(ui->actionMapUserdataEdit->text().arg(editUserpointText));
    ui->actionMapUserdataDelete->setEnabled(true);
    ui->actionMapUserdataDelete->setText(ui->actionMapUserdataDelete->text().arg(editUserpointText));
    ui->actionMapUserdataMove->setEnabled(true);
    ui->actionMapUserdataMove->setText(ui->actionMapUserdataMove->text().arg(editUserpointText));
  }
  else
  {
    ui->actionMapUserdataEdit->setText(ui->actionMapUserdataEdit->text().arg(QString()));
    ui->actionMapUserdataDelete->setText(ui->actionMapUserdataDelete->text().arg(QString()));
    ui->actionMapUserdataMove->setText(ui->actionMapUserdataMove->text().arg(QString()));
  }

  // ===================================================================================
  // Update "show in search" and "add to route" only for airports an navaids
  if(vor != nullptr || ndb != nullptr || waypoint != nullptr || airport != nullptr ||
     userpoint != nullptr)
  {
    ui->actionRouteAddPos->setEnabled(true);
    ui->actionRouteAddPos->setText(ui->actionRouteAddPos->text().arg(addRouteText));
    ui->actionRouteAppendPos->setEnabled(true);
    ui->actionRouteAppendPos->setText(ui->actionRouteAppendPos->text().arg(addRouteText));
  }
  else
  {
    ui->actionRouteAddPos->setText(ui->actionRouteAddPos->text().arg(tr("Position")));
    ui->actionRouteAppendPos->setText(ui->actionRouteAppendPos->text().arg(tr("Position")));
  }

  if(vor != nullptr || ndb != nullptr || waypoint != nullptr || airport != nullptr ||
     userpoint != nullptr || onlineAircraft != nullptr || onlineCenter != nullptr)
  {
    ui->actionShowInSearch->setEnabled(true);
    ui->actionShowInSearch->setText(ui->actionShowInSearch->text().arg(searchText));
  }
  else
    ui->actionShowInSearch->setText(ui->actionShowInSearch->text().arg(QString()));

  if(airport != nullptr)
  {
    bool hasAnyArrival = NavApp::getAirportQueryNav()->hasAnyArrivalProcedures(airport->ident);
    bool hasDeparture = NavApp::getAirportQueryNav()->hasDepartureProcedures(airport->ident);

    if(hasAnyArrival || hasDeparture)
    {
      if(airportDeparture)
      {
        if(hasDeparture)
        {
          ui->actionMapShowApproaches->setEnabled(true);
          ui->actionMapShowApproaches->setText(ui->actionMapShowApproaches->text().arg(tr("Departure ")).
                                               arg(procedureText));
        }
        else
          ui->actionMapShowApproaches->setText(tr("Show procedures (%1 has no departure procedure)").arg(airport->ident));
      }
      else if(airportDestination)
      {
        if(hasAnyArrival)
        {
          ui->actionMapShowApproaches->setEnabled(true);
          ui->actionMapShowApproaches->setText(ui->actionMapShowApproaches->text().arg(tr("Arrival ")).
                                               arg(procedureText));
        }
        else
          ui->actionMapShowApproaches->setText(tr("Show procedures (%1 has no arrival procedure)").arg(airport->ident));
      }
      else
      {
        ui->actionMapShowApproaches->setEnabled(true);
        ui->actionMapShowApproaches->setText(ui->actionMapShowApproaches->text().arg(tr("all ")).arg(procedureText));
      }
    }
    else
      ui->actionMapShowApproaches->setText(tr("Show procedures (%1 has no procedure)").arg(airport->ident));

  }
  else
  {
    ui->actionMapShowApproaches->setText(ui->actionMapShowApproaches->text().arg(QString()).arg(QString()));
  }

  if(airport != nullptr && !airport->noRunways())
  {
    ui->actionMapTrafficPattern->setEnabled(true);
    ui->actionMapTrafficPattern->setText(ui->actionMapTrafficPattern->text().arg(informationText));
  }
  else
    ui->actionMapTrafficPattern->setText(ui->actionMapTrafficPattern->text().arg(QString()));

  // Update "delete in route"
  if(routeIndex != -1 && NavApp::getRouteConst().canEditPoint(routeIndex))
  {
    ui->actionRouteDeleteWaypoint->setEnabled(true);
    ui->actionRouteDeleteWaypoint->setText(ui->actionRouteDeleteWaypoint->text().arg(routeText));
  }
  else
    ui->actionRouteDeleteWaypoint->setText(ui->actionRouteDeleteWaypoint->text().arg(tr("Position")));

  // Update "name user waypoint"
  if(routeIndex != -1 && userpointRoute != nullptr)
  {
    ui->actionMapEditUserWaypoint->setEnabled(true);
    ui->actionMapEditUserWaypoint->setText(ui->actionMapEditUserWaypoint->text().arg(routeText));
  }
  else
    ui->actionMapEditUserWaypoint->setText(ui->actionMapEditUserWaypoint->text().arg(tr("Position")));

  // Update "show range rings for Navaid"
  if(vor != nullptr || ndb != nullptr)
  {
    ui->actionMapNavaidRange->setEnabled(true);
    ui->actionMapNavaidRange->setText(ui->actionMapNavaidRange->text().arg(rangeRingText));
  }
  else
    ui->actionMapNavaidRange->setText(ui->actionMapNavaidRange->text().arg(QString()));

  if(parking == nullptr && helipad == nullptr && !measureText.isEmpty())
  {
    // Set text to measure "from airport" etc.
    ui->actionMapMeasureDistance->setText(ui->actionMapMeasureDistance->text().arg(measureText));
    ui->actionMapMeasureRhumbDistance->setText(ui->actionMapMeasureRhumbDistance->text().arg(measureText));
  }
  else
  {
    // Noting found at cursor - use "measure from here"
    ui->actionMapMeasureDistance->setText(ui->actionMapMeasureDistance->text().arg(tr("here")));
    ui->actionMapMeasureRhumbDistance->setText(ui->actionMapMeasureRhumbDistance->text().arg(tr("here")));
  }

  qDebug() << "departureParkingAirportId " << departureParkingAirportId;
  qDebug() << "airport " << airport;
  qDebug() << "vor " << vor;
  qDebug() << "ndb " << ndb;
  qDebug() << "waypoint " << waypoint;
  qDebug() << "parking " << parking;
  qDebug() << "helipad " << helipad;
  qDebug() << "routeIndex " << routeIndex;
  qDebug() << "userpointRoute " << userpointRoute;
  qDebug() << "informationText" << informationText;
  qDebug() << "procedureText" << procedureText;
  qDebug() << "measureText" << measureText;
  qDebug() << "departureText" << departureText;
  qDebug() << "destinationText" << destinationText;
  qDebug() << "addRouteText" << addRouteText;
  qDebug() << "searchText" << searchText;

  // Show the menu ------------------------------------------------
  QAction *action = menu.exec(menuPos);

  contextMenuActive = false;

  if(action != nullptr)
    qDebug() << Q_FUNC_INFO << "selected" << action->text();
  else
    qDebug() << Q_FUNC_INFO << "no action selected";

  // if(action == ui->actionMapHideRangeRings)
  // Connected to method

  if(visibleOnMap)
  {
    if(action == ui->actionShowInSearch)
    {
      // Create records and send show in search signal
      // This works only with line edit fields
      ui->dockWidgetSearch->raise();
      ui->dockWidgetSearch->show();
      if(userpoint != nullptr && !isAircraft)
      {
        ui->tabWidgetSearch->setCurrentIndex(3);
        SqlRecord rec;
        if(!userpoint->ident.isEmpty())
          rec.appendFieldAndValue("ident", userpoint->ident);
        if(!userpoint->region.isEmpty())
          rec.appendFieldAndValue("region", userpoint->region);
        if(!userpoint->name.isEmpty())
          rec.appendFieldAndValue("name", userpoint->name);
        if(!userpoint->type.isEmpty())
          rec.appendFieldAndValue("type", userpoint->type);
        if(!userpoint->tags.isEmpty())
          rec.appendFieldAndValue("tags", userpoint->tags);

        emit showInSearch(map::USERPOINT, rec);
      }
      else if(airport != nullptr && !isAircraft)
      {
        ui->tabWidgetSearch->setCurrentIndex(0);
        emit showInSearch(map::AIRPORT, SqlRecord().appendFieldAndValue("ident", airport->ident));
      }
      else if(vor != nullptr && !isAircraft)
      {
        ui->tabWidgetSearch->setCurrentIndex(1);
        SqlRecord rec;
        rec.appendFieldAndValue("ident", vor->ident);
        if(!vor->region.isEmpty())
          rec.appendFieldAndValue("region", vor->region);

        emit showInSearch(map::VOR, rec);
      }
      else if(ndb != nullptr && !isAircraft)
      {
        ui->tabWidgetSearch->setCurrentIndex(1);
        SqlRecord rec;
        rec.appendFieldAndValue("ident", ndb->ident);
        if(!ndb->region.isEmpty())
          rec.appendFieldAndValue("region", ndb->region);

        emit showInSearch(map::NDB, rec);
      }
      else if(waypoint != nullptr && !isAircraft)
      {
        ui->tabWidgetSearch->setCurrentIndex(1);
        SqlRecord rec;
        rec.appendFieldAndValue("ident", waypoint->ident);
        if(!waypoint->region.isEmpty())
          rec.appendFieldAndValue("region", waypoint->region);

        emit showInSearch(map::WAYPOINT, rec);
      }
      else if(onlineAircraft != nullptr)
      {
        ui->tabWidgetSearch->setCurrentIndex(4);
        SqlRecord rec;
        rec.appendFieldAndValue("callsign", onlineAircraft->getAirplaneRegistration());

        emit showInSearch(map::AIRCRAFT_ONLINE, rec);
      }
      else if(onlineCenter != nullptr)
      {
        ui->tabWidgetSearch->setCurrentIndex(5);
        SqlRecord rec;
        rec.appendFieldAndValue("callsign", onlineCenter->name);

        emit showInSearch(map::AIRSPACE_ONLINE, rec);
      }
    }
    else if(action == ui->actionMapNavaidRange)
    {
      if(vor != nullptr)
        addNavRangeRing(vor->position, map::VOR, vor->ident, vor->getFrequencyOrChannel(), vor->range);
      else if(ndb != nullptr)
        addNavRangeRing(ndb->position, map::NDB, ndb->ident, QString::number(ndb->frequency), ndb->range);
    }
    else if(action == ui->actionMapRangeRings)
      addRangeRing(pos);
    else if(action == ui->actionMapSetMark)
      changeSearchMark(pos);
    else if(action == ui->actionMapHideOneRangeRing)
      removeRangeRing(rangeMarkerIndex);
    else if(action == ui->actionMapHideDistanceMarker)
      removeDistanceMarker(distMarkerIndex);
    else if(action == ui->actionMapHideTrafficPattern)
      removeTrafficPatterm(trafficPatternIndex);
    else if(action == ui->actionMapMeasureDistance || action == ui->actionMapMeasureRhumbDistance)
      addMeasurement(pos, action == ui->actionMapMeasureRhumbDistance, airport, vor, ndb, waypoint);
    else if(action == ui->actionRouteDeleteWaypoint)
      NavApp::getRouteController()->routeDelete(routeIndex);
    else if(action == ui->actionMapEditUserWaypoint)
      NavApp::getRouteController()->editUserWaypointName(routeIndex);
    else if(action == ui->actionRouteAddPos || action == ui->actionRouteAppendPos ||
            action == ui->actionRouteAirportStart ||
            action == ui->actionRouteAirportDest || action == ui->actionMapShowInformation)
    {
      Pos position = pos;
      map::MapObjectTypes type = map::NONE;

      int id = -1;
      if(userpoint != nullptr)
      {
        id = userpoint->id;
        type = map::USERPOINT;
      }
      else if(airport != nullptr)
      {
        id = airport->id;
        type = map::AIRPORT;
      }
      else if(parking != nullptr)
      {
        id = parking->id;
        type = map::PARKING;
      }
      else if(helipad != nullptr)
      {
        id = helipad->id;
        type = map::HELIPAD;
      }
      else if(vor != nullptr)
      {
        id = vor->id;
        type = map::VOR;
      }
      else if(ndb != nullptr)
      {
        id = ndb->id;
        type = map::NDB;
      }
      else if(waypoint != nullptr)
      {
        id = waypoint->id;
        type = map::WAYPOINT;
      }
      else if(aiAircraft != nullptr)
      {
        id = aiAircraft->getId();
        type = map::AIRCRAFT_AI;
      }
      else if(onlineAircraft != nullptr)
      {
        id = onlineAircraft->getId();
        type = map::AIRCRAFT_ONLINE;
      }
      else if(airspace != nullptr)
      {
        id = airspace->id;
        type = map::AIRSPACE;
      }
      else if(onlineCenter != nullptr)
      {
        id = onlineCenter->id;
        type = map::AIRSPACE_ONLINE;
      }
      else if(airway != nullptr)
      {
        id = airway->id;
        type = map::AIRWAY;
      }
      else
      {
        if(userpointRoute != nullptr)
          id = userpointRoute->id;
        type = map::USERPOINTROUTE;
        position = pos;
      }

      // use airport if it is departure or destination and flight plan is visible to get quick information
      if((airportDeparture || airportDestination) && airport != nullptr && routeVisible)
      {
        id = airport->id;
        type = map::AIRPORT;
      }

      if(action == ui->actionRouteAirportStart && parking != nullptr)
        emit routeSetParkingStart(*parking);
      else if(action == ui->actionRouteAirportStart && helipad != nullptr)
        emit routeSetHelipadStart(*helipad);
      else if(action == ui->actionRouteAddPos || action == ui->actionRouteAppendPos)
      {
        if(parking != nullptr || helipad != nullptr)
        {
          // Adjust values in case user selected "add" on a parking position
          type = map::USERPOINTROUTE;
          id = -1;
        }

        if(action == ui->actionRouteAddPos)
          emit routeAdd(id, position, type, -1 /* leg index */);
        else if(action == ui->actionRouteAppendPos)
          emit routeAdd(id, position, type, map::INVALID_INDEX_VALUE);
      }
      else if(action == ui->actionRouteAirportStart)
        emit routeSetStart(*airport);
      else if(action == ui->actionRouteAirportDest)
        emit routeSetDest(*airport);
      else if(action == ui->actionMapShowInformation)
      {
        if(isAircraft)
        {
          // Aircraft have preference above all for information

          // Use same order as above in if(aiAircraft != nullptr) ...
          if(aiAircraft != nullptr)
          {
            type = map::AIRCRAFT_AI;

            if(aiAircraft->isOnlineShadow())
              // Show both online and simulator aircraft information
              type |= map::AIRCRAFT_ONLINE;
          }

          if(onlineAircraft != nullptr && !(type & map::AIRCRAFT_ONLINE))
            // Only use online if previous AI was not a shadow
            type = map::AIRCRAFT_ONLINE;

          if(userAircraft != nullptr)
          {
            type = map::AIRCRAFT;

            if(userAircraft->isOnlineShadow())
              // Show both online and simulator aircraft information
              type |= map::AIRCRAFT_ONLINE;
          }
        }
        else
          // Display only one map object as shown in the menu item
          result.clearAllButFirst();

        emit showInformation(result, type);
      }
    }
    else if(action == ui->actionMapTrafficPattern)
      addTrafficPattern(*airport);
    else if(action == ui->actionMapShowApproaches)
      emit showApproaches(*airport);
    else if(action == ui->actionMapUserdataAdd)
    {
      if(NavApp::getElevationProvider()->isGlobeOfflineProvider())
        pos.setAltitude(atools::geo::meterToFeet(NavApp::getElevationProvider()->getElevationMeter(pos)));
      emit addUserpointFromMap(result, pos);
    }
    else if(action == ui->actionMapUserdataEdit)
      emit editUserpointFromMap(result);
    else if(action == ui->actionMapUserdataDelete)
      emit deleteUserpointFromMap(userpoint->id);
    else if(action == ui->actionMapUserdataMove)
    {
      if(userpoint != nullptr)
      {
        userpointDragPixmap = *NavApp::getUserdataIcons()->getIconPixmap(userpoint->type,
                                                                         paintLayer->getMapLayer()->getUserPointSymbolSize());
        userpointDragCur = point;
        userpointDrag = *userpoint;
        // Start mouse dragging and disable context menu so we can catch the right button click as cancel
        mouseState = mw::DRAG_USER_POINT;
        setContextMenuPolicy(Qt::PreventContextMenu);
      }
    }
  }
}

void MapWidget::addTrafficPattern(const map::MapAirport& airport)
{
  qDebug() << Q_FUNC_INFO;

  TrafficPatternDialog dialog(mainWindow, airport);
  int retval = dialog.exec();
  if(retval == QDialog::Accepted)
  {
    map::TrafficPattern pattern;
    dialog.fillTrafficPattern(pattern);
    screenIndex->getTrafficPatterns().append(pattern);
    mainWindow->updateMarkActionStates();
    update();
    mainWindow->setStatusMessage(tr("Added airport traffic pattern for %1.").arg(airport.ident));
  }
}

void MapWidget::removeTrafficPatterm(int index)
{
  screenIndex->getTrafficPatterns().removeAt(index);
  mainWindow->updateMarkActionStates();
  update();
  mainWindow->setStatusMessage(QString(tr("Traffic pattern removed from map.")));
}

void MapWidget::addNavRangeRing(const atools::geo::Pos& pos, map::MapObjectTypes type,
                                const QString& ident, const QString& frequency, int range)
{
  map::RangeMarker ring;
  ring.type = type;
  ring.center = pos;

  if(type == map::VOR)
  {
    if(frequency.endsWith("X") || frequency.endsWith("Y"))
      ring.text = ident + " " + frequency;
    else
      ring.text = ident + " " + QString::number(frequency.toFloat() / 1000., 'f', 2);
  }
  else if(type == map::NDB)
    ring.text = ident + " " + QString::number(frequency.toFloat() / 100., 'f', 2);

  ring.ranges.append(range);
  screenIndex->getRangeMarks().append(ring);
  qDebug() << "navaid range" << ring.center;
  update();
  mainWindow->updateMarkActionStates();
  mainWindow->setStatusMessage(tr("Added range rings for %1.").arg(ident));
}

void MapWidget::addRangeRing(const atools::geo::Pos& pos)
{
  map::RangeMarker rings;
  rings.type = map::NONE;
  rings.center = pos;

  const QVector<int> dists = OptionData::instance().getMapRangeRings();
  for(int dist : dists)
    rings.ranges.append(atools::roundToInt(Unit::rev(dist, Unit::distNmF)));

  screenIndex->getRangeMarks().append(rings);

  qDebug() << "range rings" << rings.center;
  update();
  mainWindow->updateMarkActionStates();
  mainWindow->setStatusMessage(tr("Added range rings for position."));
}

void MapWidget::addMeasurement(const atools::geo::Pos& pos, bool rhumb, const map::MapSearchResult& result)
{
  addMeasurement(pos, rhumb,
                 atools::firstOrNull(result.airports),
                 atools::firstOrNull(result.vors),
                 atools::firstOrNull(result.ndbs),
                 atools::firstOrNull(result.waypoints));
}

void MapWidget::addMeasurement(const atools::geo::Pos& pos, bool rhumb, const map::MapAirport *airport,
                               const map::MapVor *vor, const map::MapNdb *ndb, const map::MapWaypoint *waypoint)
{
  // Distance line
  map::DistanceMarker dm;
  dm.isRhumbLine = rhumb;
  dm.to = pos;

  // Build distance line depending on selected airport or navaid (color, magvar, etc.)
  if(airport != nullptr)
  {
    dm.text = airport->name + " (" + airport->ident + ")";
    dm.from = airport->position;
    dm.magvar = airport->magvar;
    dm.color = mapcolors::colorForAirport(*airport);
  }
  else if(vor != nullptr)
  {
    if(vor->tacan)
      dm.text = vor->ident + " " + vor->channel;
    else
      dm.text = vor->ident + " " + QLocale().toString(vor->frequency / 1000., 'f', 2);
    dm.from = vor->position;
    dm.magvar = vor->magvar;
    dm.color = mapcolors::vorSymbolColor;
  }
  else if(ndb != nullptr)
  {
    dm.text = ndb->ident + " " + QLocale().toString(ndb->frequency / 100., 'f', 2);
    dm.from = ndb->position;
    dm.magvar = ndb->magvar;
    dm.color = mapcolors::ndbSymbolColor;
  }
  else if(waypoint != nullptr)
  {
    dm.text = waypoint->ident;
    dm.from = waypoint->position;
    dm.magvar = waypoint->magvar;
    dm.color = mapcolors::waypointSymbolColor;
  }
  else
  {
    dm.magvar = NavApp::getMagVar(pos, 0.f);
    dm.from = pos;
    dm.color = dm.isRhumbLine ? mapcolors::distanceRhumbColor : mapcolors::distanceColor;
  }

  screenIndex->getDistanceMarks().append(dm);

  // Start mouse dragging and disable context menu so we can catch the right button click as cancel
  mouseState = mw::DRAG_DISTANCE;
  setContextMenuPolicy(Qt::PreventContextMenu);
  currentDistanceMarkerIndex = screenIndex->getDistanceMarks().size() - 1;
}

void MapWidget::clearRangeRingsAndDistanceMarkers()
{
  qDebug() << "range rings hide";

  screenIndex->getRangeMarks().clear();
  screenIndex->getDistanceMarks().clear();
  screenIndex->getTrafficPatterns().clear();
  currentDistanceMarkerIndex = -1;

  update();
  mainWindow->updateMarkActionStates();
  mainWindow->setStatusMessage(tr("All range rings and measurement lines removed from map."));
}

void MapWidget::workOffline(bool offline)
{
  qDebug() << "Work offline" << offline;
  model()->setWorkOffline(offline);

  mainWindow->renderStatusChanged(Marble::RenderStatus::Complete);

  if(!offline)
    update();
}

void MapWidget::elevationDisplayTimerTimeout()
{
  qreal lon, lat;
  QPoint point = mapFromGlobal(QCursor::pos());

  if(rect().contains(point))
  {
    if(geoCoordinates(point.x(), point.y(), lon, lat, GeoDataCoordinates::Degree))
    {
      Pos pos(lon, lat);
      pos.setAltitude(NavApp::getElevationProvider()->getElevationMeter(pos));
      mainWindow->updateMapPosLabel(pos, point.x(), point.y());
    }
  }
}

bool MapWidget::eventFilter(QObject *obj, QEvent *e)
{
  bool jumpBackWasActive = false;

  if(e->type() == QEvent::KeyPress || e->type() == QEvent::Wheel)
    jumpBackWasActive = jumpBack->isActive();

  if(e->type() == QEvent::KeyPress)
  {
    QKeyEvent *keyEvent = dynamic_cast<QKeyEvent *>(e);
    if(keyEvent != nullptr)
    {
      if(atools::contains(static_cast<Qt::Key>(keyEvent->key()),
                          {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down}))
        // Movement starts delay every time
        jumpBackToAircraftStart(true /* save distance too */);

      if(atools::contains(static_cast<Qt::Key>(keyEvent->key()), {Qt::Key_Plus, Qt::Key_Minus}))
      {
        if(jumpBack->isActive() || isCenterLegAndAircraftActive())
          // Movement starts delay every time
          jumpBackToAircraftStart(true /* save distance too */);

        if(!jumpBackWasActive && !isCenterLegAndAircraftActive())
          // Remember and update zoom factor if jump was not active
          jumpBackToAircraftUpdateDistance();
      }

      if(atools::contains(static_cast<Qt::Key>(keyEvent->key()), {Qt::Key_Plus, Qt::Key_Minus}) &&
         (keyEvent->modifiers() & Qt::ControlModifier))
      {
        // Catch Ctrl++ and Ctrl+- and use it only for details
        // Do not let marble use it for zooming

        e->accept(); // Do not propagate further
        event(e); // Call own event handler
        return true; // Do not process further
      }
    }
  }

  if(e->type() == QEvent::Wheel)
  {
    if(jumpBack->isActive() || isCenterLegAndAircraftActive())
      // Only delay if already active. Allow zooming and jumpback if autozoom is on
      jumpBackToAircraftStart(true /* save distance too */);

    if(!jumpBackWasActive && !isCenterLegAndAircraftActive())
      // Remember and update zoom factor if jump was not active
      jumpBackToAircraftUpdateDistance();
  }

  if(e->type() == QEvent::MouseButtonDblClick)
  {
    // Catch the double click event

    e->accept(); // Do not propagate further
    event(e); // Call own event handler
    return true; // Do not process further
  }

  if(e->type() == QEvent::Wheel)
  {
    // Catch the wheel event and do own zooming since Marble is buggy

    e->accept(); // Do not propagate further
    event(e); // Call own event handler
    return true; // Do not process further
  }

  if(e->type() == QEvent::MouseButtonPress)
  {
    QMouseEvent *mouseEvent = dynamic_cast<QMouseEvent *>(e);

    if(mouseEvent != nullptr && mouseEvent->modifiers() & Qt::ControlModifier)
      // Remove control modifer to disable Marble rectangle dragging
      mouseEvent->setModifiers(mouseEvent->modifiers() & ~Qt::ControlModifier);
  }

  if(e->type() == QEvent::MouseMove)
  {
    // Update coordinate display in status bar
    QMouseEvent *mouseEvent = dynamic_cast<QMouseEvent *>(e);
    qreal lon, lat;
    if(geoCoordinates(mouseEvent->pos().x(), mouseEvent->pos().y(), lon, lat, GeoDataCoordinates::Degree))
    {
      if(NavApp::getElevationProvider()->isGlobeOfflineProvider())
        elevationDisplayTimer.start();
      mainWindow->updateMapPosLabel(Pos(lon, lat, static_cast<double>(map::INVALID_ALTITUDE_VALUE)),
                                    mouseEvent->pos().x(), mouseEvent->pos().y());
    }
    else
      mainWindow->updateMapPosLabel(Pos(), -1, -1);
  }

  if(e->type() == QEvent::MouseMove && mouseState != mw::NONE)
  {
    // Do not allow mouse scrolling during drag actions
    e->accept();
    event(e);

    // Do not process further
    return true;
  }

  QMouseEvent *mouseEvent = dynamic_cast<QMouseEvent *>(e);
  if(e->type() == QEvent::MouseMove && mouseEvent->buttons() == Qt::NoButton && mouseState == mw::NONE)
  {
    // No not pass movements to marble widget to avoid cursor fighting
    e->accept();
    event(e);
    // Do not process further
    return true;
  }

  // Pass to base class and keep on processing
  Marble::MarbleWidget::eventFilter(obj, e);
  return false;
}

void MapWidget::cancelDragAll()
{
  cancelDragRoute();
  cancelDragUserpoint();
  cancelDragDistance();

  mouseState = mw::NONE;
  setViewContext(Marble::Still);
  update();
}

/* Stop userpoint editing and reset coordinates and pixmap */
void MapWidget::cancelDragUserpoint()
{
  if(mouseState & mw::DRAG_USER_POINT)
  {
    if(cursor().shape() != Qt::ArrowCursor)
      setCursor(Qt::ArrowCursor);

    userpointDragCur = QPoint();
    userpointDrag = map::MapUserpoint();
    userpointDragPixmap = QPixmap();
  }
}

/* Stop route editing and reset all coordinates */
void MapWidget::cancelDragRoute()
{
  if(mouseState & mw::DRAG_ROUTE_POINT || mouseState & mw::DRAG_ROUTE_LEG)
  {
    if(cursor().shape() != Qt::ArrowCursor)
      setCursor(Qt::ArrowCursor);

    routeDragCur = QPoint();
    routeDragFrom = atools::geo::EMPTY_POS;
    routeDragTo = atools::geo::EMPTY_POS;
    routeDragPoint = -1;
    routeDragLeg = -1;
  }
}

/* Stop new distance line or change dragging and restore backup or delete new line */
void MapWidget::cancelDragDistance()
{
  if(cursor().shape() != Qt::ArrowCursor)
    setCursor(Qt::ArrowCursor);

  if(mouseState & mw::DRAG_DISTANCE)
    // Remove new one
    screenIndex->getDistanceMarks().removeAt(currentDistanceMarkerIndex);
  else if(mouseState & mw::DRAG_CHANGE_DISTANCE)
    // Replace modified one with backup
    screenIndex->getDistanceMarks()[currentDistanceMarkerIndex] = distanceMarkerBackup;
  currentDistanceMarkerIndex = -1;
}

#ifdef DEBUG_MOVING_AIRPLANE
void MapWidget::debugMovingPlane(QMouseEvent *event)
{
  static int packetId = 0;
  static Pos lastPos;
  static QPoint lastPoint;

  if(event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
  {
    if(QPoint(lastPoint - event->pos()).manhattanLength() > 4)
    {
      qreal lon, lat;
      geoCoordinates(event->pos().x(), event->pos().y(), lon, lat);

      float projectionDistance = NavApp::getRouteConst().getProjectionDistance();

      double alt = 100.f;
      if(projectionDistance < map::INVALID_DISTANCE_VALUE)
        alt = static_cast<double>(NavApp::getAltitudeLegs().getAltitudeForDistance(
                                    NavApp::getRoute().getTotalDistance() - projectionDistance));
      Pos pos(lon, lat, alt);

      if(pos.getAltitude() < 100.f)
        pos.setAltitude(100.f);

      if(!(pos.getAltitude() < map::INVALID_ALTITUDE_VALUE))
        pos.setAltitude(100.f);

      atools::fs::sc::SimConnectData data = atools::fs::sc::SimConnectData::buildDebugForPosition(pos, lastPos);
      data.setPacketId(packetId++);

      emit NavApp::getConnectClient()->dataPacketReceived(data);
      lastPos = pos;
      lastPoint = event->pos();
    }
  }
}

#endif

void MapWidget::mouseMoveEvent(QMouseEvent *event)
{
  if(!isActiveWindow())
    return;

#ifdef DEBUG_MOVING_AIRPLANE
  debugMovingPlane(event);
#endif

  qreal lon = 0., lat = 0.;
  bool visible = false;
  if(mouseState & mw::DRAG_ALL)
  {
    jumpBackToAircraftStart(true /* save distance too */);

    // Currently dragging a measurement line
    if(cursor().shape() != Qt::CrossCursor)
      setCursor(Qt::CrossCursor);

    visible = geoCoordinates(event->pos().x(), event->pos().y(), lon, lat);
  }

  if(mouseState & mw::DRAG_DISTANCE || mouseState & mw::DRAG_CHANGE_DISTANCE)
  {
    // Position is valid update the distance mark continuously
    if(visible && !screenIndex->getDistanceMarks().isEmpty())
      screenIndex->getDistanceMarks()[currentDistanceMarkerIndex].to = Pos(lon, lat);

  }
  else if(mouseState & mw::DRAG_ROUTE_LEG || mouseState & mw::DRAG_ROUTE_POINT)
  {
    if(visible)
      // Update current point
      routeDragCur = QPoint(event->pos().x(), event->pos().y());
  }
  else if(mouseState & mw::DRAG_USER_POINT)
  {
    if(visible)
      // Update current point
      userpointDragCur = QPoint(event->pos().x(), event->pos().y());
  }
  else if(mouseState == mw::NONE)
  {
    if(event->buttons() == Qt::NoButton)
    {
      // No dragging going on now - update cursor over flight plan legs or markers
      const Route& route = NavApp::getRouteConst();

      Qt::CursorShape cursorShape = Qt::ArrowCursor;
      bool routeEditMode = mainWindow->getUi()->actionRouteEditMode->isChecked();

      // Make distance a bit larger to prefer points
      if(routeEditMode &&
         screenIndex->getNearestRoutePointIndex(event->pos().x(), event->pos().y(),
                                                screenSearchDistance * 4 / 3) != -1 &&
         route.size() > 1)
        // Change cursor at one route point
        cursorShape = Qt::SizeAllCursor;
      else if(routeEditMode &&
              screenIndex->getNearestRouteLegIndex(event->pos().x(), event->pos().y(), screenSearchDistance) != -1 &&
              route.size() > 1)
        // Change cursor above a route line
        cursorShape = Qt::CrossCursor;
      else if(screenIndex->getNearestDistanceMarkIndex(event->pos().x(), event->pos().y(),
                                                       screenSearchDistance) != -1)
        // Change cursor at the end of an marker
        cursorShape = Qt::CrossCursor;
      else if(screenIndex->getNearestTrafficPatternIndex(event->pos().x(), event->pos().y(),
                                                         screenSearchDistance) != -1)
        // Change cursor at the end of an marker
        cursorShape = Qt::PointingHandCursor;
      else if(screenIndex->getNearestRangeMarkIndex(event->pos().x(), event->pos().y(),
                                                    screenSearchDistance) != -1)
        // Change cursor at the end of an marker
        cursorShape = Qt::PointingHandCursor;

      if(cursor().shape() != cursorShape)
        setCursor(cursorShape);
    }
    else
      jumpBackToAircraftStart(true /* save distance too */);
  }

  if(mouseState & mw::DRAG_ALL)
  {
    // Force fast updates while dragging
    setViewContext(Marble::Animation);
    update();
  }
}

void MapWidget::wheelEvent(QWheelEvent *event)
{
#ifdef DEBUG_INFORMATION
  qDebug() << Q_FUNC_INFO << "pixelDelta" << event->pixelDelta() << "angleDelta" << event->angleDelta()
           << event->source() << "geometry()" << geometry() << "event->pos()" << event->pos();
#endif

  if(!rect().contains(event->pos()))
    // Ignore wheel events that appear outside of the view and on the scrollbars
    return;

  if(event->timestamp() > lastWheelEventTimestamp + 500)
    lastWheelPos = 0;

  // Sum up wheel events
  lastWheelPos += event->angleDelta().y();

  // Check for threshold
  if(std::abs(lastWheelPos) >= 120)
  {
    bool directionIn = lastWheelPos > 0;
    lastWheelPos = 0;

    qreal lon, lat;
    if(geoCoordinates(event->pos().x(), event->pos().y(), lon, lat, GeoDataCoordinates::Degree))
    {
      // Position is visible
      qreal centerLat = centerLatitude();
      qreal centerLon = centerLongitude();

      if(event->modifiers() == Qt::ShiftModifier)
      {
        // Smooth zoom
        if(directionIn)
          zoomViewBy(zoomStep() / 4);
        else
          zoomViewBy(-zoomStep() / 4);
      }
      else
      {
        if(mainWindow->getMapThemeIndex() == map::PLAIN || mainWindow->getMapThemeIndex() == map::SIMPLE)
        {
          if(directionIn)
            zoomViewBy(zoomStep() * 3);
          else
            zoomViewBy(-zoomStep() * 3);
        }
        else
        {
          if(directionIn)
            zoomIn();
          else
            zoomOut();
        }
      }

      // Get global coordinates of cursor in new zoom level
      qreal lon2, lat2;
      geoCoordinates(event->pos().x(), event->pos().y(), lon2, lat2, GeoDataCoordinates::Degree);

      // Correct position and move center back to mouse cursor position
      centerOn(centerLon + (lon - lon2), centerLat + (lat - lat2));
    }
  }
}

void MapWidget::removeRangeRing(int index)
{
  screenIndex->getRangeMarks().removeAt(index);
  mainWindow->updateMarkActionStates();
  update();
  mainWindow->setStatusMessage(QString(tr("Range ring removed from map.")));
}

void MapWidget::removeDistanceMarker(int index)
{
  screenIndex->getDistanceMarks().removeAt(index);
  mainWindow->updateMarkActionStates();
  update();
  mainWindow->setStatusMessage(QString(tr("Measurement line removed from map.")));
}

bool MapWidget::mousePressCheckModifierActions(QMouseEvent *event)
{
  if(mouseState != mw::NONE || event->type() != QEvent::MouseButtonRelease)
    // Not if dragging or for button release
    return false;

  qreal lon, lat;
  // Cursor can be outside or map region
  if(geoCoordinates(event->pos().x(), event->pos().y(), lon, lat))
  {
    Pos pos(lon, lat);

    // Look for navaids or airports nearby click
    map::MapSearchResult result;
    screenIndex->getAllNearest(event->pos().x(), event->pos().y(), screenSearchDistance, result);

    if(event->modifiers() == Qt::ShiftModifier)
    {
      int index = screenIndex->getNearestRangeMarkIndex(event->pos().x(), event->pos().y(),
                                                        screenSearchDistance);
      if(index != -1)
        // Remove any ring for Shift+Click into center
        removeRangeRing(index);
      else
      {
        // Add rings for Shift+Click
        if(result.hasVor())
          // Add VOR range
          addNavRangeRing(pos, map::VOR, result.vors.first().ident, result.vors.first().getFrequencyOrChannel(),
                          result.vors.first().range);
        else if(result.hasNdb())
          // Add NDB range
          addNavRangeRing(pos, map::NDB, result.ndbs.first().ident, QString::number(result.ndbs.first().frequency),
                          result.ndbs.first().range);
        else
          // Add range rings per configuration
          addRangeRing(pos);
      }
      return true;
    }
    else if(event->modifiers() == (Qt::AltModifier | Qt::ControlModifier) ||
            event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))
    {
      int routeIndex = screenIndex->getNearestRoutePointIndex(event->pos().x(), event->pos().y(), screenSearchDistance);

      if(routeIndex != -1)
      {
        // Position is editable - remove
        NavApp::getRouteController()->routeDelete(routeIndex);
        return true;
      }

      if(event->modifiers() == (Qt::AltModifier | Qt::ControlModifier))
      {
        // Add to nearest leg of flight plan
        updateRoute(event->pos(), -1, -1, true /* click add */, false /* click append */);
        return true;
      }
      else if(event->modifiers() == (Qt::AltModifier | Qt::ShiftModifier))
      {
        // Append to flight plan
        updateRoute(event->pos(), -1, -1, false /* click add */, true /* click append */);
        return true;
      }
    }
    else if(event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
    {
      // Add or edit userpoint
      if(result.hasUserpoints())
        emit editUserpointFromMap(result);
      else
      {
        if(NavApp::getElevationProvider()->isGlobeOfflineProvider())
          pos.setAltitude(atools::geo::meterToFeet(NavApp::getElevationProvider()->getElevationMeter(pos)));
        emit addUserpointFromMap(result, pos);
      }
    }
    else if(event->modifiers() == Qt::ControlModifier || event->modifiers() == Qt::AltModifier)
    {
      int index = screenIndex->getNearestDistanceMarkIndex(event->pos().x(), event->pos().y(),
                                                           screenSearchDistance);
      if(index != -1)
        // Remove any measurement line for Ctrl+Click or Alt+Click into center
        removeDistanceMarker(index);
      else
        // Add measurement line for Ctrl+Click or Alt+Click into center
        addMeasurement(pos, event->modifiers() == Qt::ControlModifier, result);
      return true;
    }
  }
  return false;
}

void MapWidget::mousePressEvent(QMouseEvent *event)
{
#ifdef DEBUG_MOVING_AIRPLANE
  debugMovingPlane(event);
#endif

  hideTooltip();

  jumpBackToAircraftStart(true /* save distance too */);

  // Remember mouse position to check later if mouse was moved during click (drag map scroll)
  mouseMoved = event->pos();
  if(mouseState & mw::DRAG_ALL)
  {
    if(cursor().shape() != Qt::ArrowCursor)
      setCursor(Qt::ArrowCursor);

    if(event->button() == Qt::LeftButton)
      // Done with any dragging
      mouseState |= mw::DRAG_POST;
    else if(event->button() == Qt::RightButton)
      // Cancel any dragging
      mouseState |= mw::DRAG_POST_CANCEL;
  }
  else if(mouseState == mw::NONE && event->buttons() & Qt::RightButton)
    // First right click after dragging - enable context menu again
    setContextMenuPolicy(Qt::DefaultContextMenu);
  else
  {
    // No drag and drop mode - use hand to indicate scrolling
    if(event->button() == Qt::LeftButton && cursor().shape() != Qt::OpenHandCursor)
      setCursor(Qt::OpenHandCursor);
  }
}

void MapWidget::mouseReleaseEvent(QMouseEvent *event)
{
  qDebug() << Q_FUNC_INFO
           << "state" << mouseState
           << "modifiers" << event->modifiers()
           << "pos" << event->pos();

  // Take actions (add/remove range rings, measurement)
  if(mousePressCheckModifierActions(event))
    // Event was consumed - do not proceed here
    return;

  hideTooltip();

  jumpBackToAircraftStart(true /* save distance too */);

  if(mouseState & mw::DRAG_ROUTE_POINT || mouseState & mw::DRAG_ROUTE_LEG)
  {
    // End route point dragging
    if(mouseState & mw::DRAG_POST)
    {
      // Ending route dragging - update route
      qreal lon, lat;
      bool visible = geoCoordinates(event->pos().x(), event->pos().y(), lon, lat);
      if(visible)
        updateRoute(routeDragCur, routeDragLeg, routeDragPoint, false /* click add */, false /* click append */);
    }

    // End all dragging
    cancelDragRoute();
    mouseState = mw::NONE;
    setViewContext(Marble::Still);
    update();
  }
  else if(mouseState & mw::DRAG_DISTANCE || mouseState & mw::DRAG_CHANGE_DISTANCE)
  {
    // End distance marker dragging
    if(!screenIndex->getDistanceMarks().isEmpty())
    {
      setCursor(Qt::ArrowCursor);
      if(mouseState & mw::DRAG_POST)
      {
        qreal lon, lat;
        bool visible = geoCoordinates(event->pos().x(), event->pos().y(), lon, lat);
        if(visible)
          // Update distance measurement line
          screenIndex->getDistanceMarks()[currentDistanceMarkerIndex].to = Pos(lon, lat);
      }
      else if(mouseState & mw::DRAG_POST_CANCEL)
        cancelDragDistance();
    }
    else
    {
      if(cursor().shape() != Qt::ArrowCursor)
        setCursor(Qt::ArrowCursor);
    }

    mouseState = mw::NONE;
    setViewContext(Marble::Still);
    update();
  }
  else if(mouseState & mw::DRAG_USER_POINT)
  {
    // End userpoint dragging
    if(mouseState & mw::DRAG_POST)
    {
      // Ending route dragging - update route
      qreal lon, lat;
      bool visible = geoCoordinates(event->pos().x(), event->pos().y(), lon, lat);
      if(visible)
      {
        // Create a copy before cancel
        map::MapUserpoint newUserpoint = userpointDrag;
        newUserpoint.position = Pos(lon, lat);
        emit moveUserpointFromMap(newUserpoint);
      }
    }

    cancelDragUserpoint();

    // End all dragging
    mouseState = mw::NONE;
    setViewContext(Marble::Still);
    update();

  }
  else if(event->button() == Qt::LeftButton && (event->pos() - mouseMoved).manhattanLength() < 4)
  {
    // Start all dragging if left button was clicked and mouse was not moved
    currentDistanceMarkerIndex = screenIndex->getNearestDistanceMarkIndex(event->pos().x(),
                                                                          event->pos().y(),
                                                                          screenSearchDistance);
    if(currentDistanceMarkerIndex != -1)
    {
      // Found an end - create a backup and start dragging
      mouseState = mw::DRAG_CHANGE_DISTANCE;
      distanceMarkerBackup = screenIndex->getDistanceMarks().at(currentDistanceMarkerIndex);
      setContextMenuPolicy(Qt::PreventContextMenu);
    }
    else
    {
      if(mainWindow->getUi()->actionRouteEditMode->isChecked())
      {
        const Route& route = NavApp::getRouteConst();

        if(route.size() > 1)
        {
          // Make distance a bit larger to prefer points
          int routePoint =
            screenIndex->getNearestRoutePointIndex(event->pos().x(), event->pos().y(),
                                                   screenSearchDistance * 4 / 3);
          if(routePoint != -1)
          {
            routeDragPoint = routePoint;
            qDebug() << "route point" << routePoint;

            // Found a leg - start dragging
            mouseState = mw::DRAG_ROUTE_POINT;

            routeDragCur = QPoint(event->pos().x(), event->pos().y());

            if(routePoint > 0)
              routeDragFrom = route.at(routePoint - 1).getPosition();
            else
              routeDragFrom = atools::geo::EMPTY_POS;

            if(routePoint < route.size() - 1)
              routeDragTo = route.at(routePoint + 1).getPosition();
            else
              routeDragTo = atools::geo::EMPTY_POS;
            setContextMenuPolicy(Qt::PreventContextMenu);
          }
          else
          {
            int routeLeg = screenIndex->getNearestRouteLegIndex(event->pos().x(),
                                                                event->pos().y(), screenSearchDistance);
            if(routeLeg != -1)
            {
              routeDragLeg = routeLeg;
              qDebug() << "route leg" << routeLeg;
              // Found a leg - start dragging
              mouseState = mw::DRAG_ROUTE_LEG;

              routeDragCur = QPoint(event->pos().x(), event->pos().y());

              routeDragFrom = route.at(routeLeg).getPosition();
              routeDragTo = route.at(routeLeg + 1).getPosition();
              setContextMenuPolicy(Qt::PreventContextMenu);
            }
          }
        }
      }

      if(mouseState == mw::NONE)
      {
        if(cursor().shape() != Qt::ArrowCursor)
          setCursor(Qt::ArrowCursor);
        handleInfoClick(event->pos());
      }
    }
  }
  else
  {
    // No drag and drop mode - switch back to arrow after scrolling
    if(cursor().shape() != Qt::ArrowCursor)
      setCursor(Qt::ArrowCursor);
  }

  mouseMoved = QPoint();
  mainWindow->updateMarkActionStates();
}

void MapWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
  qDebug() << Q_FUNC_INFO;

  // Show pos and show rect already call this
  // jumpBackToAircraftStart();

  if(mouseState != mw::NONE)
    return;

  hideTooltip();

  map::MapSearchResult mapSearchResult;
  screenIndex->getAllNearest(event->pos().x(), event->pos().y(), screenSearchDistance, mapSearchResult);

  if(mapSearchResult.userAircraft.isValid())
  {
    showPos(mapSearchResult.userAircraft.getPosition(), 0.f, true);
    mainWindow->setStatusMessage(QString(tr("Showing user aircraft on map.")));
  }
  else if(!mapSearchResult.aiAircraft.isEmpty())
  {
    showPos(mapSearchResult.aiAircraft.first().getPosition(), 0.f, true);
    mainWindow->setStatusMessage(QString(tr("Showing AI / multiplayer aircraft on map.")));
  }
  else if(!mapSearchResult.onlineAircraft.isEmpty())
  {
    showPos(mapSearchResult.onlineAircraft.first().getPosition(), 0.f, true);
    mainWindow->setStatusMessage(QString(tr("Showing online client aircraft on map.")));
  }
  else if(!mapSearchResult.airports.isEmpty())
  {
    showRect(mapSearchResult.airports.first().bounding, true);
    mainWindow->setStatusMessage(QString(tr("Showing airport on map.")));
  }
  else
  {
    if(!mapSearchResult.vors.isEmpty())
      showPos(mapSearchResult.vors.first().position, 0.f, true);
    else if(!mapSearchResult.ndbs.isEmpty())
      showPos(mapSearchResult.ndbs.first().position, 0.f, true);
    else if(!mapSearchResult.waypoints.isEmpty())
      showPos(mapSearchResult.waypoints.first().position, 0.f, true);
    else if(!mapSearchResult.userPointsRoute.isEmpty())
      showPos(mapSearchResult.userPointsRoute.first().position, 0.f, true);
    else if(!mapSearchResult.userpoints.isEmpty())
      showPos(mapSearchResult.userpoints.first().position, 0.f, true);
    mainWindow->setStatusMessage(QString(tr("Showing navaid or userpoint on map.")));
  }
}

/* Stop all line drag and drop if the map looses focus */
void MapWidget::focusOutEvent(QFocusEvent *)
{
  hideTooltip();

  if(!(mouseState & mw::DRAG_POST_MENU))
  {
    cancelDragAll();
    setContextMenuPolicy(Qt::DefaultContextMenu);
  }
}

void MapWidget::leaveEvent(QEvent *)
{
  hideTooltip();
  mainWindow->updateMapPosLabel(Pos(), -1, -1);
}

void MapWidget::keyPressEvent(QKeyEvent *event)
{
  // Does not work for key presses that are consumed by the widget

  if(event->key() == Qt::Key_Escape)
  {
    cancelDragAll();
    setContextMenuPolicy(Qt::DefaultContextMenu);
  }

  if(event->key() == Qt::Key_Menu && mouseState == mw::NONE)
    // First menu key press after dragging - enable context menu again
    setContextMenuPolicy(Qt::DefaultContextMenu);
}

const QList<int>& MapWidget::getRouteHighlights() const
{
  return screenIndex->getRouteHighlights();
}

const QList<map::RangeMarker>& MapWidget::getRangeRings() const
{
  return screenIndex->getRangeMarks();
}

const QList<map::DistanceMarker>& MapWidget::getDistanceMarkers() const
{
  return screenIndex->getDistanceMarks();
}

const QList<map::TrafficPattern>& MapWidget::getTrafficPatterns() const
{
  return screenIndex->getTrafficPatterns();
}

void MapWidget::hideTooltip()
{
  QToolTip::hideText();
  tooltipPos = QPoint();
}

void MapWidget::updateTooltip()
{
  showTooltip(true /* update */);
}

void MapWidget::showTooltip(bool update)
{
  // qDebug() << Q_FUNC_INFO << "update" << update << "QToolTip::isVisible()" << QToolTip::isVisible();

  if(databaseLoadStatus)
    return;

  // Try to avoid spurious tooltip events
  if(update && !QToolTip::isVisible())
    return;

  // Build a new tooltip HTML for weather changes or aircraft updates
  QString text = mapTooltip->buildTooltip(mapSearchResultTooltip, procPointsTooltip, NavApp::getRouteConst(),
                                          paintLayer->getMapLayer()->isAirportDiagram());

  if(!text.isEmpty() && !tooltipPos.isNull())
    QToolTip::showText(tooltipPos, text /*, nullptr, QRect(), 3600 * 1000*/);
  else
    hideTooltip();
}

const atools::fs::sc::SimConnectUserAircraft& MapWidget::getUserAircraft() const
{
  return screenIndex->getUserAircraft();
}

const QVector<atools::fs::sc::SimConnectAircraft>& MapWidget::getAiAircraft() const
{
  return screenIndex->getAiAircraft();
}

void MapWidget::deleteAircraftTrack()
{
  aircraftTrack.clearTrack();
  emit updateActionStates();
  update();
  mainWindow->setStatusMessage(QString(tr("Aircraft track removed from map.")));
}

bool MapWidget::event(QEvent *event)
{
  if(event->type() == QEvent::ToolTip)
  {
    QHelpEvent *helpEvent = static_cast<QHelpEvent *>(event);

    // Load tooltip data into mapSearchResultTooltip
    mapSearchResultTooltip = map::MapSearchResult();
    procPointsTooltip.clear();
    screenIndex->getAllNearest(helpEvent->pos().x(), helpEvent->pos().y(), screenSearchDistanceTooltip,
                               mapSearchResultTooltip, procPointsTooltip);
    NavApp::getOnlinedataController()->filterOnlineShadowAircraft(mapSearchResultTooltip.onlineAircraft,
                                                                  mapSearchResultTooltip.aiAircraft);

    tooltipPos = helpEvent->globalPos();

    // Build HTML
    showTooltip(false /* update */);
    event->accept();
    return true;
  }

  return QWidget::event(event);
}

void MapWidget::paintEvent(QPaintEvent *paintEvent)
{
  if(!active)
  {
    QPainter painter(this);
    painter.fillRect(paintEvent->rect(), QGuiApplication::palette().color(QPalette::Window));
    return;
  }

  bool changed = false;
  const GeoDataLatLonAltBox visibleLatLonAltBox = viewport()->viewLatLonAltBox();

  if(viewContext() == Marble::Still &&
     (zoom() != currentZoom || visibleLatLonAltBox != currentViewBoundingBox))
  {
    // This paint event has changed zoom or the visible bounding box
    currentZoom = zoom();
    currentViewBoundingBox = visibleLatLonAltBox;

    // qDebug() << "paintEvent map view has changed zoom" << currentZoom
    // << "distance" << distance() << " (" << meterToNm(distance() * 1000.) << " km)";

    if(!noStoreInHistory)
      // Not changed by next/last in history
      history.addEntry(Pos(centerLongitude(), centerLatitude()), distance());

    noStoreInHistory = false;
    changed = true;
  }

  MarbleWidget::paintEvent(paintEvent);

  if(changed)
  {
    // Major change - update index and visible objects
    mapVisible->updateVisibleObjectsStatusBar();
    screenIndex->updateRouteScreenGeometry(currentViewBoundingBox);
    screenIndex->updateAirwayScreenGeometry(currentViewBoundingBox);
    screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
  }

  if(paintLayer->getOverflow() > 0)
    emit resultTruncated(paintLayer->getOverflow());
}

void MapWidget::handleInfoClick(QPoint pos)
{
  qDebug() << Q_FUNC_INFO << pos;

  map::MapSearchResult result;
  screenIndex->getAllNearest(pos.x(), pos.y(), screenSearchDistance, result);

  // Remove all undesired features
  opts::DisplayClickOptions opts = OptionData::instance().getDisplayClickOptions();
  if(!(opts & opts::CLICK_AIRPORT))
  {
    result.airports.clear();
    result.airportIds.clear();
  }

  if(!(opts & opts::CLICK_NAVAID))
  {
    result.vors.clear();
    result.vorIds.clear();
    result.ndbs.clear();
    result.ndbIds.clear();
    result.waypoints.clear();
    result.waypointIds.clear();
    result.airways.clear();
    result.userpoints.clear();
  }

  if(!(opts & opts::CLICK_AIRSPACE))
    result.airspaces.clear();

  emit showInformation(result, map::NONE);
}

bool MapWidget::loadKml(const QString& filename, bool center)
{
  if(QFile::exists(filename))
  {
    model()->addGeoDataFile(filename, 0, center && OptionData::instance().getFlags() & opts::GUI_CENTER_KML);

    if(center)
      showAircraft(false);
    return true;
  }
  return false;
}

void MapWidget::defaultMapDetail()
{
  mapDetailLevel = MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR;
  setMapDetail(mapDetailLevel);
}

void MapWidget::increaseMapDetail()
{
  if(mapDetailLevel < MapLayerSettings::MAP_MAX_DETAIL_FACTOR)
  {
    mapDetailLevel++;
    setMapDetail(mapDetailLevel);
  }
}

void MapWidget::decreaseMapDetail()
{
  if(mapDetailLevel > MapLayerSettings::MAP_MIN_DETAIL_FACTOR)
  {
    mapDetailLevel--;
    setMapDetail(mapDetailLevel);
  }
}

void MapWidget::setMapDetail(int factor)
{
  Ui::MainWindow *ui = mainWindow->getUi();

  mapDetailLevel = factor;
  setDetailLevel(mapDetailLevel);
  ui->actionMapMoreDetails->setEnabled(mapDetailLevel < MapLayerSettings::MAP_MAX_DETAIL_FACTOR);
  ui->actionMapLessDetails->setEnabled(mapDetailLevel > MapLayerSettings::MAP_MIN_DETAIL_FACTOR);
  ui->actionMapDefaultDetails->setEnabled(mapDetailLevel != MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR);
  update();

  int det = mapDetailLevel - MapLayerSettings::MAP_DEFAULT_DETAIL_FACTOR;
  QString detStr;
  if(det == 0)
    detStr = tr("Normal");
  else if(det > 0)
    detStr = "+" + QString::number(det);
  else if(det < 0)
    detStr = QString::number(det);

  // Update status bar label
  mainWindow->setDetailLabelText(tr("Detail %1").arg(detStr));
  mainWindow->setStatusMessage(tr("Map detail level changed."));
}

void MapWidget::onlineClientAndAtcUpdated()
{
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
  update();
}

void MapWidget::onlineNetworkChanged()
{
  screenIndex->resetAirspaceOnlineScreenGeometry();
  screenIndex->updateAirspaceScreenGeometry(currentViewBoundingBox);
  update();
}

void MapWidget::takeoffLandingTimeout()
{
  const atools::fs::sc::SimConnectUserAircraft aircraft = screenIndex->getLastUserAircraft();

  if(aircraft.isFlying())
  {
    // In air after  status has changed
    qDebug() << Q_FUNC_INFO << "Takeoff detected" << aircraft.getZuluTime();

    takeoffLandingDistanceNm = 0.;
    takeoffLandingAverageTasKts = aircraft.getTrueAirspeedKts();
    takeoffLastSampleTimeMs = takeoffTimeMs = aircraft.getZuluTime().toMSecsSinceEpoch();

    emit aircraftTakeoff(aircraft);
  }
  else
  {
    // On ground after status has changed
    qDebug() << Q_FUNC_INFO << "Landing detected takeoffLandingDistanceNm" << takeoffLandingDistanceNm;
    emit aircraftLanding(aircraft,
                         static_cast<float>(takeoffLandingDistanceNm),
                         static_cast<float>(takeoffLandingAverageTasKts));
  }
}

void MapWidget::jumpBackToAircraftUpdateDistance()
{
  QVariantList values = jumpBack->getValues();
  if(values.size() == 3)
  {
    values.replace(2, distance());
    jumpBack->updateValues(values);
  }
}

void MapWidget::jumpBackToAircraftStart(bool saveDistance)
{
  if(NavApp::getMainUi()->actionMapAircraftCenter->isChecked())
  {
    if(jumpBack->isActive())
      // Simply restart
      jumpBack->restart();
    else
      // Start and save coordinates
      jumpBack->start({centerLongitude(), centerLatitude(), saveDistance ? distance() : QVariant(QVariant::Double)});
  }
}

void MapWidget::jumpBackToAircraftCancel()
{
  jumpBack->cancel();
}

void MapWidget::jumpBackToAircraftTimeout(const QVariantList& values)
{
  if(NavApp::getMainUi()->actionMapAircraftCenter->isChecked() && NavApp::isConnectedAndAircraft() &&
     OptionData::instance().getFlags2() & opts::ROUTE_NO_FOLLOW_ON_MOVE)
  {

    if(mouseState != mw::NONE || viewContext() == Marble::Animation || contextMenuActive)
      // Restart as long as menu is active or user is dragging around
      jumpBack->restart();
    else
    {
      jumpBack->cancel();

      hideTooltip();
      centerPosOnMap(Pos(values.at(0).toFloat(), values.at(1).toFloat()));

      if(values.size() > 2)
      {
        QVariant distVar = values.at(2);
        if(distVar.isValid() && !distVar.isNull())
          setDistanceToMap(distVar.toDouble(), false /* Allow adjust zoom */);
      }
      mainWindow->setStatusMessage(tr("Jumped back to aircraft."));
    }
  }
  else
    jumpBack->cancel();
}
