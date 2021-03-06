/************************************************************************
    MeOS - Orienteering Software
    Copyright (C) 2009-2016 Melin Software HB

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Melin Software HB - software@melin.nu - www.melin.nu
    Eksoppsv�gen 16, SE-75646 UPPSALA, Sweden

************************************************************************/

#include "stdafx.h"

#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>

#include "oEvent.h"
#include "xmlparser.h"
#include "gdioutput.h"
#include "csvparser.h"
#include "SportIdent.h"
#include "meos_util.h"
#include "TabCompetition.h"
#include "TabCourse.h"
#include "oFreeImport.h"
#include "localizer.h"
#include "oListInfo.h"
#include "download.h"
#include "progress.h"
#include "classconfiginfo.h"
#include "RunnerDB.h"
#include "gdifonts.h"
#include "meosException.h"
#include "meosdb/sqltypes.h"
#include "socket.h"
#include "iof30interface.h"
#include "MeOSFeatures.h"
#include "prefseditor.h"
#include "recorder.h"
#include "testmeos.h"

#include <Shellapi.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <io.h>

void Setup(bool overwrite, bool overWriteall);
void exportSetup();
void resetSaveTimer();
extern bool enableTests;

int ListsCB(gdioutput *gdi, int type, void *data);

TabCompetition::TabCompetition(oEvent *poe):TabBase(poe)
{
  eventorBase = poe->getPropertyString("EventorBase", "https://eventor.orientering.se/api/");
  iofExportVersion = "&version=3.0";
  defaultServer="localhost";
  defaultName="meos";
  organizorId = 0;
  lastSelectedClass = -1;
  allTransfer.insert(-1);
  lastChangeClassType = oEvent::ChangeClassVacant;
}

TabCompetition::~TabCompetition(void)
{
}

extern SportIdent *gSI;
extern HINSTANCE hInst;
extern HWND hWndMain;

bool TabCompetition::save(gdioutput &gdi, bool write)
{
  string name=gdi.getText("Name");

  if (name.empty()) {
    gdi.alert("T�vlingen m�ste ha ett namn");
    return 0;
  }

  string zt = gdi.getText("ZeroTime");
  bool longTimes = gdi.isChecked("LongTimes");
  string date = gdi.getText("Date");

  if (longTimes)
    zt = "00:00:00";

  int newZT = convertAbsoluteTimeHMS(zt, -1);
  if (newZT < 0)
    throw meosException("Felaktigt tidsformat 'X' (Anv�nd TT:MM:SS)#" + zt);

  int oldZT = convertAbsoluteTimeHMS(oe->getZeroTime(), -1);
  bool oldLT = oe->useLongTimes();
  string oldDate = oe->getDate();
  
  if ((newZT != oldZT || 
      longTimes != oldLT || 
      (longTimes && date != oldDate)) && oe->classHasResults(0)) {
    if (!gdi.ask("warn:changedtimezero")) {
      gdi.setText("ZeroTime", oe->getZeroTime());
      gdi.check("LongTimes", oe->useLongTimes());
      gdi.setText("Date", oe->getDate());
      return 0;
    }
  }
  oe->setDate(date);
  oe->useLongTimes(longTimes);
  oe->setName(gdi.getText("Name"));
  oe->setAnnotation(gdi.getText("Annotation"));
  oe->setZeroTime(zt);

  oe->synchronize();
  if (gSI) gSI->SetZeroTime(oe->getZeroTimeNum());

  gdi.setWindowTitle(oe->getTitleName());
  gdi.setText("Date", oe->getDate());
  gdi.setText("ZeroTime", oe->getZeroTime());

  if (write) {
    gdi.setWaitCursor(true);
    resetSaveTimer();
    return oe->save();
  }
  else
    return true;
}

bool TabCompetition::importFile(HWND hWnd, gdioutput &gdi)
{
  vector< pair<string, string> > ext;
  ext.push_back(make_pair("xml-data", "*.xml;*.bu?"));
  string fileName = gdi.browseForOpen(ext, "xml");
  if (fileName.empty())
    return false;

  gdi.setWaitCursor(true);
  if (oe->open(fileName, true)) {
    if (gSI) gSI->SetZeroTime(oe->getZeroTimeNum());
    gdi.setWindowTitle(oe->getTitleName());
    resetSaveTimer();
    return true;
  }

  return false;
}

bool TabCompetition::exportFileAs(HWND hWnd, gdioutput &gdi)
{
  int ix = 0;
  vector< pair<string, string> > ext;
  ext.push_back(make_pair("xml-data", "*.xml"));
  string fileName = gdi.browseForSave(ext, "xml", ix);
  if (fileName.empty())
    return false;

  gdi.setWaitCursor(true);
  if (!oe->save(fileName.c_str())) {
    gdi.alert("Fel: Filen " + fileName+ " kunde inte skrivas.");
    return false;
  }

  return true;
}

int CompetitionCB(gdioutput *gdi, int type, void *data)
{
  TabCompetition &tc = dynamic_cast<TabCompetition &>(*gdi->getTabs().get(TCmpTab));

  return tc.competitionCB(*gdi, type, data);
}


int restoreCB(gdioutput *gdi, int type, void *data)
{
  TabCompetition &tc = dynamic_cast<TabCompetition &>(*gdi->getTabs().get(TCmpTab));

  return tc.restoreCB(*gdi, type, data);
}

void TabCompetition::loadConnectionPage(gdioutput &gdi)
{
  gdi.clearPage(false);
  showConnectionPage=true;
  gdi.addString("", boldLarge, "Anslutningar");

  if (oe->getServerName().empty()) {
    gdi.addString("", 10, "help:52726");
    gdi.pushX();
    gdi.dropLine();
    defaultServer = oe->getPropertyString("Server", defaultServer);
    defaultName = oe->getPropertyString("UserName", defaultName);
    defaultPort = oe->getPropertyString("Port", defaultPort);
    string client = oe->getPropertyString("Client", oe->getClientName());

    gdi.fillRight();
    gdi.addInput("Server", defaultServer, 16, 0, "MySQL Server / IP-adress:", "IP-adress eller namn p� en MySQL-server");
    gdi.addInput("UserName", defaultName, 7, 0, "Anv�ndarnamn:");
    gdi.addInput("PassWord", defaultPwd, 9, 0, "L�senord:").setPassword(true);
    gdi.addInput("Port", defaultPort, 4, 0, "Port:");

    if (defaultServer.empty())
      gdi.setInputFocus("Server");
    else if (defaultName.empty())
      gdi.setInputFocus("UserName");
    else
      gdi.setInputFocus("PassWord");

    gdi.fillDown();
    gdi.popX();
    gdi.dropLine(2.5);
    gdi.addInput("ClientName", client, 16, 0, "Klientnamn:");
    gdi.dropLine();
    gdi.fillRight();
    gdi.addButton("ConnectToMySQL", "Anslut", CompetitionCB).setDefault();
  }
  else {
    gdi.addString("", 10, "help:50431");
    gdi.dropLine(1);
    gdi.pushX();
    gdi.fillRight();
    gdi.addString("", 1, "Ansluten till:");
    gdi.addStringUT(1, oe->getServerName()).setColor(colorGreen);
    gdi.popX();
    gdi.dropLine(2);
    gdi.addInput("ClientName", oe->getClientName(), 16, 0, "Klientnamn:");
    gdi.dropLine();
    gdi.addButton("SaveClient", "�ndra", CompetitionCB);
    gdi.dropLine(2.5);

    gdi.popX();
    gdi.addString("", 1, "�ppnad t�vling:");

    if (oe->empty())
      gdi.addString("", 1, "Ingen").setColor(colorRed);
    else {
      gdi.addStringUT(1, oe->getName()).setColor(colorGreen);

      if (oe->isClient())
        gdi.addString("", 1, "(p� server)");
      else
        gdi.addString("", 1, "(lokalt)");

    }
    gdi.dropLine(2);
    gdi.popX();
    gdi.fillRight();

    if (!oe->isClient())
      gdi.addButton("UploadCmp", "Ladda upp �ppnad t�vling p� server",CompetitionCB);

    if (oe->empty()) {
      gdi.disableInput("UploadCmp");
    }
    else {
      gdi.addButton("CloseCmp", "St�ng t�vlingen", CompetitionCB);
      gdi.addButton("Delete", "Radera t�vlingen", CompetitionCB);
    }
    gdi.dropLine(2);
    gdi.popX();
    if (oe->empty()) {
      char bf[260];
      getUserFile(bf, "");
      oe->enumerateCompetitions(bf, "*.meos");

      gdi.dropLine(1);
      gdi.fillRight();
      gdi.addListBox("ServerCmp", 320, 210,  CompetitionCB, "Server");
      oe->fillCompetitions(gdi, "ServerCmp", 2);
      gdi.selectItemByData("ServerCmp", oe->getPropertyInt("LastCompetition", 0));

      gdi.fillDown();
      gdi.addListBox("LocalCmp", 320, 210, CompetitionCB, "Lokalt");
      gdi.popX();
      oe->fillCompetitions(gdi, "LocalCmp", 1);
      gdi.selectItemByData("LocalCmp", oe->getPropertyInt("LastCompetition", 0));

      gdi.addCheckbox("UseDirectSocket", "Skicka och ta emot snabb f�rhandsinformation om st�mplingar och resultat",
                      0, oe->getPropertyInt("UseDirectSocket", true) != 0);

      gdi.dropLine();
      gdi.fillRight();
      gdi.addButton("OpenCmp", "�ppna t�vling", CompetitionCB).setDefault();
      gdi.addButton("Repair", "Reparera vald t�vling", CompetitionCB);

      gdi.setInputStatus("Repair", gdi.getSelectedItem("ServerCmp").second, true);
    }
    else if (oe->isClient()) {
      gdi.fillDown();
      gdi.popX();
      oe->listConnectedClients(gdi);
      gdi.registerEvent("Connections", CompetitionCB);
      gdi.fillRight();
    }

    gdi.addButton("DisconnectMySQL", "Koppla ner databas", CompetitionCB);
  }
  gdi.addButton("Cancel", "Till huvudsidan", CompetitionCB).setCancel();
  gdi.fillDown();
  gdi.refresh();
}

bool TabCompetition::checkEventor(gdioutput &gdi, ButtonInfo &bi) {
  eventorOrigin = bi.id;

  if (organizorId == 0) {
    int clubId = getOrganizer(true);
    if (clubId == 0) {
      bi.id = "EventorAPI";
      competitionCB(gdi, GUI_BUTTON, &bi);
      return true;
    }
    else if (clubId == -1)
      throw std::exception("Kunde inte ansluta till Eventor");

    organizorId = clubId;
  }
  return false;
}

int eventorServer(gdioutput *gdi, int type, void *data) {
  TabCompetition &tc = dynamic_cast<TabCompetition &>(*gdi->getTabs().get(TCmpTab));
  if (type == GUI_COMBO) {
    const ListBoxInfo &lbi = *((ListBoxInfo *)data);
    tc.setEventorServer(lbi.text);
  }
  else if (type == GUI_BUTTON) {
    const ButtonInfo &bi = *((ButtonInfo *)data);

    if (bi.id == "EventorUTC")
      tc.setEventorUTC(gdi->isChecked(bi.id));
  }
  return 0;
}

void TabCompetition::setEventorServer(const string &server) {
  eventorBase = server;
  oe->setProperty("EventorBase", server);
}

void TabCompetition::setEventorUTC(bool useUTC) {
  oe->setProperty("UseEventorUTC", useUTC);
}

bool TabCompetition::useEventorUTC() const {
  bool eventorUTC = oe->getPropertyInt("UseEventorUTC", 0) != 0;
  return eventorUTC;
}

enum StartMethod {SMCommon = 1, SMDrawn, SMFree, SMCustom};

int TabCompetition::competitionCB(gdioutput &gdi, int type, void *data)
{
  if (type == GUI_LINK) {
    TextInfo ti = *(TextInfo *)data;
    if (ti.id == "link") {
      string url = ti.text;
      ShellExecute(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
  }
  else if (type==GUI_BUTTON) {
    ButtonInfo bi=*(ButtonInfo *)data;

    if (bi.id == "CopyLink") {
      string url = gdi.getText("link");

      if (OpenClipboard(gdi.getHWND())) {
        EmptyClipboard();
        HGLOBAL hClipboardData;
        hClipboardData = GlobalAlloc(GMEM_DDESHARE,
                                     url.length()+1);

        char * pchData;
        pchData = (char*)GlobalLock(hClipboardData);

        strcpy_s(pchData, url.length()+1, LPCSTR(url.c_str()));

        GlobalUnlock(hClipboardData);

        SetClipboardData(CF_TEXT,hClipboardData);
        CloseClipboard();
      }
    }
    else if (bi.id == "LongTimes") {
      if (gdi.isChecked(bi.id)) {
        gdi.setTextTranslate("ZeroTimeHelp", "help:long_times", true);
        gdi.disableInput("ZeroTime");
      }
      else {
        gdi.setTextTranslate("ZeroTimeHelp", "help:zero_time", true);
        gdi.enableInput("ZeroTime");
      }
    }
    else if (bi.id=="Print") {
      gdi.print(oe);
    }
    else if (bi.id=="Setup") {
      gdi.clearPage(false);

      gdi.addString("", boldLarge, "Inst�llningar MeOS");
      gdi.dropLine();
      gdi.addString("", 10, "help:29191");
      gdi.dropLine();
      char FileNamePath[260];
      getUserFile(FileNamePath, "");
      gdi.addStringUT(0, lang.tl("MeOS lokala datakatalog �r") + ": " + FileNamePath);
      gdi.dropLine();

      gdi.addCombo("EventorServer", 320, 100, eventorServer, "Eventor server:");

      vector<string> eventorCand;
      eventorCand.push_back("https://eventor.orientering.se/api/");

      gdi.addItem("EventorServer", eventorBase);
      for (size_t k = 0; k < eventorCand.size(); k++) {
        if (eventorBase != eventorCand[k])
          gdi.addItem("EventorServer", eventorCand[k]);
      }

      gdi.selectFirstItem("EventorServer");

      bool eventorUTC = oe->getPropertyInt("UseEventorUTC", 0) != 0;
      gdi.addCheckbox("EventorUTC", "Eventors tider i UTC (koordinerad universell tid)", eventorServer, eventorUTC);

      char bf[260];
      GetCurrentDirectory(260, bf);
      gdi.fillRight();
      gdi.pushX();
      gdi.dropLine();
      gdi.addInput("Source", bf, 40, 0, "K�llkatalog:");
      gdi.dropLine(0.8);
      gdi.addButton("SourceBrowse", "Bl�ddra...", CompetitionCB);
      gdi.dropLine(4);
      gdi.popX();
      gdi.fillRight();
      gdi.addButton("DoSetup", "Installera", CompetitionCB);
      gdi.addButton("ExportSetup", "Exportera", CompetitionCB);
      gdi.addButton("Cancel", "St�ng", CompetitionCB);
      gdi.dropLine(2);
      gdi.refresh();
    }
    else if (bi.id=="SourceBrowse") {
      string s = gdi.browseForFolder(gdi.getText("Source"), 0);
      if (!s.empty())
        gdi.setText("Source", s);
    }
    else if (bi.id=="DoSetup") {
      string source = gdi.getText("Source");
      if (SetCurrentDirectory(source.c_str())) {
        Setup(true, true);
        gdi.alert("Tillg�ngliga filer installerades. Starta om MeOS.");
        exit(0);
      }
      else
        throw std::exception("Operationen misslyckades");
    }
    else if (bi.id == "RunnerDatabase") {
      loadRunnerDB(gdi, 0, false);
      return 0;
    }
    else if (bi.id=="ExportSetup") {

      gdi.clearPage(false);
      gdi.addString("", boldLarge, "Exportera inst�llningar och l�pardatabaser");
      gdi.dropLine();
      gdi.addString("", 10, "help:15491");
      gdi.dropLine();
      char FileNamePath[260];
      getUserFile(FileNamePath, "");
      gdi.addStringUT(0, lang.tl("MeOS lokala datakatalog �r: ") + FileNamePath);

      gdi.dropLine();

      char bf[260];
      GetCurrentDirectory(260, bf);
      gdi.fillRight();
      gdi.pushX();
      gdi.dropLine();
      gdi.addInput("Source", bf, 40, 0, "Destinationskatalog:");
      gdi.dropLine(0.8);
      gdi.addButton("SourceBrowse", "Bl�ddra...", CompetitionCB);
      gdi.dropLine(4);
      gdi.popX();
      gdi.fillRight();
      gdi.addButton("DoExportSetup", "Exportera", CompetitionCB).setDefault();
      gdi.addButton("CancelRunnerDatabase", "Avbryt", CompetitionCB).setCancel();
      gdi.dropLine(2);
      gdi.refresh();
    }
    else if (bi.id=="DoExportSetup") {
      string source = gdi.getText("Source");
      if (SetCurrentDirectory(source.c_str())) {
        exportSetup();
        gdi.alert("Inst�llningarna har exporterats.");
      }
    }
    else if (bi.id == "SaveTest") {
      vector< pair<string, string> > cpp;
      cpp.push_back(make_pair("Source", "*.cpp"));
      int ix = 0;
      string fn = gdi.browseForSave(cpp, ".cpp", ix);
      if (!fn.empty())
        gdi.getRecorder().saveRecordings(fn);
    }
    else if (bi.id == "RunTest") {
      TestMeOS tm(oe, "base");
      tm.runAll();
      oe->clear();
      gdi.selectTab(tabId);
      tm.publish(gdi);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id == "RunSpecificTest") {
      int test = gdi.getSelectedItem("Tests").first;
      TestMeOS tm(oe, "base");
      tm.runSpecific(test);
    }
    else if (bi.id == "LocalSettings") {
      gdi.clearPage(false);
      gdi.addString("", boldLarge, "�ndra MeOS lokala systemegenskaper");
      gdi.dropLine(0.5);
      gdi.addString("", 0, "Vissa inst�llningar kr�ver omstart av MeOS f�r att ha effekt.");
      gdi.dropLine(0.5);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.dropLine();

      if (prefsEditor.empty())
        prefsEditor.push_back(PrefsEditor(oe));

      prefsEditor.back().showPrefs(gdi);
      gdi.refresh();
    }
    else if (bi.id=="Test") {
      vector< pair<string, string> > cpp;
      cpp.push_back(make_pair("Source", "*.cpp"));
      int ix = 0;
      string fn = gdi.browseForSave(cpp, ".cpp", ix);
      if (!fn.empty())
        gdi.getRecorder().saveRecordings(fn);
      else {
        TestMeOS tm(oe, "base");
        tm.runAll();
        tm.publish(gdi);
      }
      /*gdi.clearPage(false);
      gdi.addString("", boldLarge, "�ndra MeOS lokala systemegenskaper");
      gdi.dropLine(0.5);
      gdi.addString("", 0, "Vissa inst�llningar kr�ver omstart av MeOS f�r att ha effekt.");
      gdi.dropLine(0.5);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.dropLine();

      if (prefsEditor.empty())
        prefsEditor.push_back(PrefsEditor(oe));

      prefsEditor.back().showPrefs(gdi);
      gdi.refresh();*/
    }
    else if (bi.id=="Report") {
      gdi.clearPage(true);
      oe->generateCompetitionReport(gdi);

      gdi.addButton(gdi.getWidth()+20, 15, gdi.scaleLength(120), "Cancel",
                    "�terg�", CompetitionCB,  "", true, false);
      gdi.addButton(gdi.getWidth()+20, 18+gdi.getButtonHeight(), gdi.scaleLength(120), "Print",
                    "Skriv ut...", CompetitionCB,  "Skriv ut rapporten", true, false);
      gdi.refresh();

      //gdi.addButton("Cancel", "Avbryt", CompetitionCB);
    }
    else if (bi.id=="Features") {
      save(gdi, false);
      meosFeatures(gdi, false);
    }
    else if (bi.id == "SaveFeaures") {
      saveMeosFeatures(gdi, true);
      loadPage(gdi);
    }
    else if (bi.id=="Settings") {
      loadSettings(gdi);
    }
    else if (bi.id == "UseFraction") {
      gdi.setInputStatus("CurrencySeparator_odc", gdi.isChecked(bi.id));
    }
    else if (bi.id == "AddPayMode") {
      saveSettings(gdi);
      vector< pair<string, size_t> > modes;
      oe->getPayModes(modes);
      oe->setPayMode(modes.size(), lang.tl("Betals�tt"));
      loadSettings(gdi);
    }
    else if (bi.id == "RemovePayMode") {
      saveSettings(gdi);
      oe->setPayMode(bi.getExtraInt(), "");
      loadSettings(gdi);
    }
    else if (bi.id=="SaveSettings") {
      saveSettings(gdi);
      loadPage(gdi);
    }
    else if (bi.id == "Exit") {
      PostMessage(gdi.getMain(), WM_CLOSE, 0, 0);
    }
    else if (bi.id == "Help") {
      char fn[MAX_PATH];
      getMeOSFile(fn, lang.tl("documentation").c_str());
      if (_access(fn, 0)==-1) {
        gdi.alert(string("Hittar inte hj�lpfilen, X#") + fn);
        return 0;
      }

      gdi.openDoc(fn);
    }
    else if (bi.id=="Browse") {
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("IOF (xml)", "*.xml"));

      string f = gdi.browseForOpen(ext, "xml");
      string id;
      if (!f.empty()) {
        InputInfo &ii = dynamic_cast<InputInfo &>(gdi.getBaseInfo(bi.getExtra()));
        gdi.setText(ii.id, f);
      }
    }
	else if (bi.id == "BrowseOE") {
		vector< pair<string, string> > ext;
		ext.push_back(make_pair("OE (csv)", "*.csv"));

		string f = gdi.browseForOpen(ext, "csv");
		string id;
		if (!f.empty()) {
			InputInfo &ii = dynamic_cast<InputInfo &>(gdi.getBaseInfo(bi.getExtra()));
			gdi.setText(ii.id, f);
		}
	}
	else if (bi.id=="DBaseIn") {
      gdi.clearPage(true);
      gdi.addString("", boldLarge, "Importera l�pare och klubbar / distriktsregister");
      gdi.dropLine();
      gdi.addString("", 10, "help:runnerdatabase");
      gdi.dropLine(2);
      gdi.pushX();
      gdi.fillRight();
      gdi.addInput("ClubFile", "", 40, 0, "Filnamn IOF (xml) med klubbar");
      gdi.dropLine();
      gdi.addButton("Browse", "Bl�ddra...", CompetitionCB).setExtra("ClubFile");
	  gdi.popX();
	  gdi.dropLine(3);
	  gdi.addInput("CmpFile", "", 40, 0, "Filnamn IOF (xml) med l�pare");
	  gdi.dropLine();
	  gdi.addButton("Browse", "Bl�ddra...", CompetitionCB).setExtra("CmpFile");
	  gdi.popX();
	  gdi.dropLine(3);
	  gdi.addInput("OECSVFile", "", 40, 0, "Filename OE (csv) with runners and clubs");
	  gdi.dropLine();
	  gdi.addButton("BrowseOE", "Bl�ddra...", CompetitionCB).setExtra("OECSVFile");

      gdi.dropLine(2);

      gdi.popX();
	  gdi.addCheckbox("Clear", "Nollst�ll databaser", 0, true);
	  gdi.addCheckbox("ReverseNames", "Import names as \"surname, first name\"", 0, true);
	  gdi.dropLine(1.5);
	  gdi.popX();
	  gdi.addCheckbox("UseFFCOClubMapping", "Use French Federation of Orienteering mapping", 0, oe->getPropertyString("Language", "English") == "Fran�ais");
	  gdi.dropLine(3);

      gdi.popX();
      gdi.addButton("DoDBaseIn", "Importera", CompetitionCB).setDefault();
      gdi.addButton("CancelRunnerDatabase", "Avbryt", CompetitionCB).setCancel();
      gdi.dropLine(3);
      gdi.fillDown();
      gdi.popX();
    }
    else if (bi.id=="DoDBaseIn") {
      gdi.enableEditControls(false);
      gdi.disableInput("DoDBaseIn");
      gdi.disableInput("CancelRunnerDatabase");

      gdi.setWaitCursor(true);
      gdi.addString("", 0, "Importerar...");
      bool clear = gdi.isChecked("Clear");
	  bool reverseNames = gdi.isChecked("ReverseNames");
	  bool useFFCOClubMapping = gdi.isChecked("UseFFCOClubMapping");

	  if (strlen(gdi.getText("ClubFile").c_str()) != 0 || strlen(gdi.getText("CmpFile").c_str()) != 0) {
		  oe->importXML_IOF_Data(gdi.getText("ClubFile").c_str(),
			  gdi.getText("CmpFile").c_str(), clear);
	  }
	  else {
		  oe->importOECSV_Data(gdi.getText("OECSVFile").c_str(), clear, reverseNames, useFFCOClubMapping);
	  }
      gdi.dropLine();
      gdi.addButton("CancelRunnerDatabase", "�terg�", CompetitionCB);
      gdi.refresh();
      gdi.setWaitCursor(false);
    }
    else if (bi.id=="Reset") {
      if (gdi.ask("Vill d� �terst�lla inst�llningar och skriva �ver egna databaser?"))
        Setup(true, true);
    }
    else if (bi.id=="ConnectMySQL")
      loadConnectionPage(gdi);
    else if (bi.id=="SaveClient") {
      oe->setClientName(gdi.getText("ClientName"));
      if (gdi.getText("ClientName").length()>0)
          oe->setProperty("Client", gdi.getText("ClientName"));
    }
    else if (bi.id=="ConnectToMySQL") {
      bool s=oe->connectToMySQL(gdi.getText("Server"),
                                gdi.getText("UserName"),
                                gdi.getText("PassWord"),
                                gdi.getTextNo("Port"));

      if (s) {
        defaultServer=gdi.getText("Server");
        defaultName=gdi.getText("UserName");
        defaultPwd=gdi.getText("PassWord");
        defaultPort=gdi.getText("Port");

        oe->setClientName(gdi.getText("ClientName"));
        oe->setProperty("Server", defaultServer);
        oe->setProperty("UserName", defaultName);
        oe->setProperty("Port", defaultPort);
        if (gdi.getText("ClientName").length()>0)
          oe->setProperty("Client", gdi.getText("ClientName"));


        loadConnectionPage(gdi);
      }
    }
    else if (bi.id == "Repair") {
      if (!gdi.ask("ask:repair"))
        return 0;
      ListBoxInfo lbi;
      int id=0;
      if ( gdi.getSelectedItem("ServerCmp", lbi) )
        id=lbi.data;
      else
        throw meosException("Ingen t�vling vald.");

      string nameId = oe->getNameId(id);
      vector<string> output;
      repairTables(nameId, output);
      gdi.clearPage(true);
      gdi.addString("", boldLarge, "Reparerar t�vlingsdatabasen");
      gdi.dropLine();
      for (size_t k = 0; k < output.size(); k++) {
        gdi.addStringUT(0, output[k]);
      }
      gdi.dropLine();
      gdi.addButton("Cancel", "Klart", CompetitionCB);

    }
    else if (bi.id=="DisconnectMySQL") {
      oe->closeDBConnection();
      loadConnectionPage(gdi);
    }
    else if (bi.id=="UploadCmp") {
      if (oe->uploadSynchronize())
        gdi.setWindowTitle(oe->getTitleName());

      if (oe->isClient() && oe->getPropertyInt("UseDirectSocket", true) != 0) {
        oe->getDirectSocket().startUDPSocketThread(gdi.getMain());
      }

      loadConnectionPage(gdi);
    }
    else if (bi.id == "MultiEvent") {
      loadMultiEvent(gdi);
    }
    else if (bi.id == "CloneEvent") {
      string ne = oe->cloneCompetition(true, false, false, false, false);
      oe->updateTabs(true);
    }
    else if (bi.id == "CloneCmp") {
      gdi.restore("MultiHeader");
      gdi.dropLine(3);
      gdi.fillDown();
      gdi.addString("", 1, "Skapar ny etapp").setColor(colorGreen);
      gdi.addString("", 0, "�verf�r anm�lda");
      gdi.refreshFast();
      string ne = oe->cloneCompetition(true, false, false, false, true);


      gdi.addString("", 0, "Klart");
      gdi.dropLine();

      char bf[260];
      getUserFile(bf, "");
      oe->enumerateCompetitions(bf, "*.meos");
      oe->updateTabs(true);
      gdi.addButton("MultiEvent", "�terg�", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id == "SaveMulti") {
      saveMultiEvent(gdi);
    }
    else if (bi.id == "OpenPost" || bi.id == "OpenPre") {
      
      saveMultiEvent(gdi);

      string nameId = oe->getNameId(0);
      ListBoxInfo lbi;
      bool openPost = false;
      
      int theNumber = oe->getStageNumber();

      if (bi.id == "OpenPost") {
        gdi.getSelectedItem("PostEvent", lbi);
        openPost = true;
        if (theNumber == 0) {
          oe->setStageNumber(1);
          theNumber = 1;
        }
        theNumber++;
      }
      else {
        gdi.getSelectedItem("PreEvent", lbi);
        if (theNumber == 0) {
          oe->setStageNumber(2);
          theNumber = 2;
        }
        theNumber--;
      }

      int id = lbi.data;

      if (id>0) {
        oe->save();
        openCompetition(gdi, id);
        oe->getMeOSFeatures().useFeature(MeOSFeatures::SeveralStages, true, *oe);
        if (openPost) {
          oe->getDI().setString("PreEvent", nameId);
          if (theNumber > 1) {
            oe->setStageNumber(theNumber);
          }
        }
        else {
          oe->getDI().setString("PostEvent", nameId);
          if (theNumber >= 0) {
            oe->setStageNumber(theNumber);
          }
        }
        loadMultiEvent(gdi);
      }
    }
    else if (bi.id == "TransferData") {
      saveMultiEvent(gdi);

      ListBoxInfo lbi;
      gdi.getSelectedItem("PostEvent", lbi);
      if (int(lbi.data) == -2)
        throw std::exception("N�sta etapp �r odefinierad.");

      gdi.clearPage(true);
      gdi.addString("", boldLarge, "�verf�r resultat till n�sta etapp");
      gdi.setData("PostEvent", lbi.data);
      gdi.dropLine();
      selectTransferClasses(gdi, false);
    }
    else if (bi.id == "SelectAll" || bi.id=="SelectNone") {
      set<int> s;
      if (bi.id=="SelectAll")
        s.insert(-1);
      gdi.setSelection("ClassNewEntries", s);
    }
    else if (bi.id == "ExpandTResults") {
      selectTransferClasses(gdi, true);
    }
    else if (bi.id == "DoTransferData") {
      bool transferNoCompet = true;
      gdi.disableInput("DoTransferData");
      gdi.disableInput("MultiEvent");
      gdi.disableInput("ExpandTResults", true);
      gdi.disableInput("SelectAll", true);
      gdi.disableInput("SelectNone", true);
      if (gdi.hasField("ClassNewEntries")) {
        gdi.getSelection("ClassNewEntries", allTransfer);
        transferNoCompet = gdi.isChecked("TransferEconomy");
      }
      else {
        //oe->getAllClasses(allTransfer);
        allTransfer.clear();
        transferNoCompet = false;
      }
      int id = (int)gdi.getData("PostEvent");
      oEvent::ChangedClassMethod method = oEvent::ChangedClassMethod(gdi.getSelectedItem("ChangeClassType").first);
      lastChangeClassType = method;

      string file = oe->getFileNameFromId(id);

      bool success = false;
      oEvent nextStage(gdi);

      if (!file.empty())
        success = nextStage.open(file.c_str(), false);

      if (success)
        success = nextStage.getNameId(0) == oe->getDCI().getString("PostEvent");

      if (success) {
        gdi.enableEditControls(false);
        gdi.dropLine(3);
        gdi.fillDown();
        gdi.addString("", 1, "�verf�r resultat till X#" + nextStage.getName());
        gdi.refreshFast();

        vector<pRunner> changedClass, changedClassNoResult, assignedVacant,newEntries,notTransfered, failedTarget;

        oe->transferResult(nextStage, allTransfer,  method, transferNoCompet,
                           changedClass, changedClassNoResult, assignedVacant, 
                           newEntries, notTransfered, failedTarget);
        bool fixedProblem = false;

        if (!changedClass.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare har bytt klass:");
          displayRunners(gdi, changedClass);
        }

        if (!changedClassNoResult.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare har bytt klass (inget totalresultat):");
          displayRunners(gdi, changedClassNoResult);
        }


        if (!assignedVacant.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare har tilldelats en vakant plats:");
          displayRunners(gdi, assignedVacant);
        }

        if (!newEntries.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare �r nyanm�lda:");
          displayRunners(gdi, newEntries);
        }

        if (!notTransfered.empty() && transferNoCompet) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare deltar ej:");
          displayRunners(gdi, notTransfered);
        }
        else if (!notTransfered.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare �verf�rdes ej:");
          displayRunners(gdi, notTransfered);
        }

        if (!failedTarget.empty()) {
          fixedProblem = true;
          gdi.dropLine();
          gdi.addString("", 1, "F�ljande deltagare �r anm�lda till n�sta etapp men inte denna:");
          displayRunners(gdi, failedTarget);
        }

        vector<pTeam> newEntriesT, notTransferedT, failedTargetT;
        oe->transferResult(nextStage, method, newEntriesT, notTransferedT, failedTargetT);

        nextStage.save();
        oe->updateTabs(true);
        gdi.dropLine();

        if (!fixedProblem) {
          gdi.addString("", 1, "Samtliga deltagare tilldelades resultat.").setColor(colorGreen);
        }
        else {
          gdi.addString("", 1, "Klart.").setColor(colorGreen);
        }

        gdi.dropLine();
        gdi.fillRight();
        gdi.addButton("MultiEvent", "�terg�", CompetitionCB);
        gdi.scrollToBottom();
        gdi.refresh();

      }
      else
        throw std::exception("Kunde inte lokalisera n�sta etapp");
    }
    else if (bi.id == "UseEventor") {
      if (gdi.isChecked("UseEventor"))
        oe->setProperty("UseEventor", 1);
      else
        oe->setProperty("UseEventor", 2);
      PostMessage(gdi.getTarget(), WM_USER + 2, TCmpTab, 0);
    }
    else if (bi.id == "EventorAPI") {
      assert(!eventorOrigin.empty());
      //DWORD d;
      //if (gdi.getData("ClearPage", d))
      gdi.clearPage(true);
      gdi.addString("", boldLarge, "Nyckel f�r Eventor");
      gdi.dropLine();
      gdi.addString("", 10, "help:eventorkey");
      gdi.dropLine();
      gdi.addInput("apikey", "", 40, 0, "API-nyckel:");
      gdi.dropLine();
      gdi.fillRight();
      gdi.pushX();
      gdi.setRestorePoint("APIKey");
      gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();
      gdi.addButton("EventorAPISave", "Spara", CompetitionCB).setDefault();
      gdi.dropLine(3);
      gdi.popX();
    }
    else if (bi.id == "EventorAPISave") {
      string key = gdi.getText("apikey");
      oe->setPropertyEncrypt("apikey", key);

      int clubId = getOrganizer(false);

      if (clubId > 0) {
        gdi.restore("APIKey", false);
        gdi.fillDown();
        gdi.addString("", 1, "Godk�nd API-nyckel").setColor(colorGreen);
        gdi.addString("", 0, "Klubb: X#" + eventor.name);
        gdi.addStringUT(0, eventor.city);
        gdi.dropLine();
        gdi.addButton("APIKeyOK", "Forts�tt", CompetitionCB);
        gdi.refresh();
      }
      else {
        gdi.fillDown();
        gdi.dropLine();
        oe->setPropertyEncrypt("apikey", "");
        organizorId = 0;
        gdi.addString("", boldText, "Felaktig nyckel").setColor(colorRed);
        gdi.refresh();
      }
    }
    else if (bi.id == "APIKeyOK") {
      oe->setProperty("Organizer", eventor.name);
      string adr  = eventor.careOf.empty() ? eventor.street :
                      eventor.careOf + ", " + eventor.street;
      oe->setProperty("Street", adr);
      oe->setProperty("Address", eventor.zipCode + " " + eventor.city);
      if (eventor.account.size() > 0)
        oe->setProperty("Account", eventor.account);
      if (eventor.email.size() > 0)
        oe->setProperty("EMail", eventor.email);
      assert(!eventorOrigin.empty());
      bi.id = eventorOrigin;
      eventorOrigin.clear();
      return competitionCB(gdi, type, &bi);
    }
    else if (bi.id == "EventorUpdateDB") {
      gdi.clearPage(false);
      gdi.addString("", boldLarge, "Uppdatera l�pardatabasen");
      gdi.setData("UpdateDB", 1);
      bi.id = "EventorImport";
      return competitionCB(gdi, type, &bi);
    }
    else if (bi.id == "SynchEventor") {
      if (checkEventor(gdi, bi))
        return 0;

      gdi.clearPage(true);
      //gdi.setData("EventorId", (int)oe->getExtIdentifier());
      //gdi.setData("UpdateDB", 1);
      gdi.addString("", boldLarge, "Utbyt t�vlingsdata med Eventor");
      gdi.dropLine();

      ClassConfigInfo cnf;
      oe->getClassConfigurationInfo(cnf);

      gdi.fillRight();
      gdi.addButton("EventorEntries", "H�mta efteranm�lningar", CompetitionCB);
      gdi.addButton("EventorUpdateDB", "Uppdatera l�pardatabasen", CompetitionCB);
      gdi.addButton("EventorStartlist", "Publicera startlista", CompetitionCB, "Publicera startlistan p� Eventor");

      if (!cnf.hasStartTimes())
        gdi.disableInput("EventorStartlist");

      gdi.addButton("EventorResult", "Publicera resultat", CompetitionCB, "Publicera resultat och str�cktider p� Eventor och WinSplits online");

      if (!cnf.hasResults())
        gdi.disableInput("EventorResult");

      gdi.addButton("Cancel", "Avbryt", CompetitionCB);
      gdi.popX();
      gdi.dropLine(2);
      bi.id = "EventorImport";
      //competitionCB(gdi, type, &bi);
    }
    else if (bi.id == "EventorEntries") {
      ClassConfigInfo cnf;
      oe->getClassConfigurationInfo(cnf);
      if (cnf.hasResults()) {
        if (!gdi.ask("T�vlingen har redan resultat. Vill du verkligen h�mta anm�lningar?"))
          return 0;
      }
      gdi.enableEditControls(false);
      gdi.enableInput("Cancel");
      gdi.dropLine(2);
      gdi.setData("EventorId", (int)oe->getExtIdentifier());
      gdi.setData("UpdateDB", DWORD(0));
      bi.id = "EventorImport";
      competitionCB(gdi, type, &bi);
    }
    else if (bi.id == "EventorStartlist") {
      gdi.clearPage(true);
      gdi.fillDown();
      gdi.dropLine();
      gdi.addString("", boldLarge, "Publicerar startlistan");

      gdi.dropLine();
      gdi.fillDown();
      gdi.addString("", 1, "Ansluter till Internet").setColor(colorGreen);

      gdi.refreshFast();
      Download dwl;
      dwl.initInternet();

      string startlist = getTempFile();
      bool eventorUTC = oe->getPropertyInt("UseEventorUTC", 0) != 0;
      oe->exportIOFStartlist(oEvent::IOF30, startlist.c_str(), eventorUTC, set<int>(), false, false);
      vector<string> fileList;
      fileList.push_back(startlist);

      string zipped = getTempFile();
      zip(zipped.c_str(), 0, fileList);
      ProgressWindow pw(gdi.getTarget());
      pw.init();
      vector<pair<string,string> > key;
      getAPIKey(key);

      string result = getTempFile();
      try {
        dwl.postFile(eventorBase + "import/startlist", zipped, result, key, pw);
      }
      catch (std::exception &ex) {
        gdi.fillRight();
        gdi.pushX();
        gdi.addString("", 1, "Operationen misslyckades: ");
        gdi.addString("", 0, ex.what()).setColor(colorRed);
        gdi.dropLine(2);
        gdi.popX();
        gdi.addButton("Cancel", "Avbryt", CompetitionCB);
        gdi.addButton(bi.id, "F�rs�k igen", CompetitionCB);
        removeTempFile(startlist);
        removeTempFile(zipped);
        gdi.refresh();
        return 0;
      }

      removeTempFile(startlist);
      removeTempFile(zipped);
      gdi.addString("", 1, "Klart");

      xmlparser xml(0);
      xml.read(result.c_str());
      xmlobject obj = xml.getObject("ImportStartListResult");
      if (obj) {
        string url;
        obj.getObjectString("StartListUrl", url);
        if (url.length()>0) {
          gdi.fillRight();
          gdi.pushX();
          gdi.dropLine();
          gdi.addString("", 0, "L�nk till startlistan:");
          gdi.addString("link", 0, url, CompetitionCB).setColor(colorRed);
        }
      }
      gdi.dropLine(3);
      gdi.popX();

      gdi.addButton("CopyLink", "Kopiera l�nken till urklipp", CompetitionCB);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id == "EventorResult") {
      ClassConfigInfo cnf;
      oe->getClassConfigurationInfo(cnf);
      if (cnf.hasPatrol()) {
        if (!gdi.ask("N�r denna version av MeOS sl�pptes kunde Eventor "
                     "inte hantera resultat fr�n patrullklasser. Vill du f�rs�ka �nd�?"))
          return loadPage(gdi);
      }

      gdi.clearPage(true);
      gdi.fillDown();
      gdi.dropLine();
      gdi.addString("", boldLarge, "Publicerar resultat");

      gdi.dropLine();
      gdi.fillDown();
      gdi.addString("", 1, "Ansluter till Internet").setColor(colorGreen);

      gdi.refreshFast();
      Download dwl;
      dwl.initInternet();

      string resultlist = getTempFile();
      set<int> classes;
      bool eventorUTC = oe->getPropertyInt("UseEventorUTC", 0) != 0;
      oe->exportIOFSplits(oEvent::IOF30, resultlist.c_str(), false,
                          eventorUTC, classes, -1, false, true, false);
      vector<string> fileList;
      fileList.push_back(resultlist);

      string zipped = getTempFile();
      zip(zipped.c_str(), 0, fileList);
      ProgressWindow pw(gdi.getTarget());
      pw.init();
      vector<pair<string,string> > key;
      getAPIKey(key);

      string result = getTempFile();

      try {
        dwl.postFile(eventorBase + "import/resultlist", zipped, result, key, pw);
      }
      catch (std::exception &ex) {
        gdi.fillRight();
        gdi.pushX();
        gdi.addString("", 1, "Operationen misslyckades: ");
        gdi.addString("", 0, ex.what()).setColor(colorRed);
        gdi.dropLine(2);
        gdi.popX();
        gdi.addButton("Cancel", "Avbryt", CompetitionCB);
        gdi.addButton(bi.id, "F�rs�k igen", CompetitionCB);
        removeTempFile(resultlist);
        removeTempFile(zipped);
        gdi.refresh();
        return 0;
      }

      removeTempFile(resultlist);
      removeTempFile(zipped);
      gdi.addString("", 1, "Klart");

      xmlparser xml(0);
      xml.read(result.c_str());
      xmlobject obj = xml.getObject("ImportResultListResult");
      if (obj) {
        string url;
        obj.getObjectString("ResultListUrl", url);
        if (url.length()>0) {
          gdi.fillRight();
          gdi.pushX();
          gdi.dropLine();
          gdi.addString("", 0, "L�nk till resultatlistan:");
          gdi.addString("link", 0, url, CompetitionCB).setColor(colorRed);
        }
      }
      gdi.dropLine(3);
      gdi.popX();

      gdi.addButton("CopyLink", "Kopiera l�nken till urklipp", CompetitionCB);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id == "Eventor") {
      if (checkEventor(gdi, bi))
        return 0;

      SYSTEMTIME st;
      GetLocalTime(&st);
      st.wYear--; // Include last years competitions
      getEventorCompetitions(gdi, convertSystemDate(st),  events);
      gdi.clearPage(true);

      gdi.addString("", boldLarge, "H�mta data fr�n Eventor");

      gdi.dropLine();
      gdi.addButton("EventorAPI", "Anslutningsinst�llningar...", CompetitionCB);
      gdi.dropLine();
      gdi.fillRight();
      gdi.pushX();
      gdi.addCheckbox("EventorCmp", "H�mta t�vlingsdata", CompetitionCB, true);
      gdi.addSelection("EventorSel", 300, 200);
      sort(events.begin(), events.end());
      st.wYear++; // Restore current time
      string now = convertSystemDate(st);

      int selected = 0; // Select next event by default
      for (int k = events.size()-1; k>=0; k--) {
        string n = events[k].Name + " (" + events[k].Date + ")";
        gdi.addItem("EventorSel", n, k);
        if (now < events[k].Date || selected == 0)
          selected = k;
      }
      gdi.selectItemByData("EventorSel", selected);

      gdi.dropLine(3);
      gdi.popX();
      gdi.addCheckbox("EventorDb", "Uppdatera l�pardatabasen", CompetitionCB, true);
      gdi.dropLine(3);
      gdi.popX();
      gdi.addButton("Cancel", "Avbryt", CompetitionCB);
      gdi.addButton("EventorNext", "N�sta >>", CompetitionCB);
    }
    else if (bi.id == "EventorCmp") {
      gdi.setInputStatus("EventorSel", gdi.isChecked(bi.id));
      gdi.setInputStatus("EventorNext", gdi.isChecked(bi.id) | gdi.isChecked("EventorDb"));
    }
    else if (bi.id == "EventorDb") {
      gdi.setInputStatus("EventorNext", gdi.isChecked(bi.id) | gdi.isChecked("EventorCmp"));
    }
    else if (bi.id == "EventorNext") {
      bool cmp = gdi.isChecked("EventorCmp");
      bool db = gdi.isChecked("EventorDb");
      ListBoxInfo lbi;
      gdi.getSelectedItem("EventorSel", lbi);
      const CompetitionInfo *ci = 0;
      if (lbi.data < events.size())
        ci = &events[lbi.data];

      gdi.clearPage(true);
      gdi.setData("UpdateDB", db);
      gdi.pushX();
      if (cmp && ci) {
        gdi.setData("EventIndex", lbi.data);
        gdi.setData("EventorId", ci->Id);
        gdi.addString("", boldLarge, "H�mta t�vlingsdata f�r X#" + ci->Name);
        gdi.dropLine(0.5);

        gdi.fillRight();
        gdi.pushX();

        int tt = convertAbsoluteTimeHMS(ci->firstStart, -1);
        string ttt = tt>0 ? ci->firstStart : "";
        gdi.addInput("FirstStart", ttt, 10, 0, "F�rsta ordinarie starttid:", "Skriv f�rsta starttid p� formen HH:MM:SS");

        gdi.addSelection("StartType", 200, 150, 0, "Startmetod", "help:startmethod");
        gdi.addItem("StartType", lang.tl("Gemensam start"), SMCommon);
        gdi.addItem("StartType", lang.tl("Lottad startlista"), SMDrawn);
        gdi.addItem("StartType", lang.tl("Fria starttider"), SMFree);
        gdi.addItem("StartType", lang.tl("Jag sk�ter lottning sj�lv"), SMCustom);
        gdi.selectFirstItem("StartType");
        gdi.fillDown();
        gdi.popX();
        gdi.dropLine(3);

        gdi.addInput("LastEntryDate", ci->lastNormalEntryDate, 10, 0, "Sista ordinarie anm�lningsdatum:");

        if (oe->getNumRunners() > 0) {
          gdi.addCheckbox("RemoveRemoved", "Ta bort eventuella avanm�lda deltagare", 0, true);
        }

        gdi.addString("", boldText, "Importera banor");
        gdi.addString("", 10, "help:ocad13091");
        gdi.fillRight();
        gdi.dropLine();
        gdi.addInput("FileName", "", 48, 0, "Filnamn (OCAD banfil):");
        gdi.dropLine();
        gdi.fillDown();
        gdi.addButton("BrowseCourse", "Bl�ddra...", CompetitionCB);
      }
      else {
        gdi.addString("", boldLarge, "H�mta l�pardatabasen");
        gdi.dropLine(0.5);

        bi.id = "EventorImport";
        return competitionCB(gdi, type, &bi);
      }

      gdi.dropLine(1);
      gdi.popX();
      gdi.setRestorePoint("DoEventor");
      gdi.fillRight();
      gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();
      gdi.addButton("EventorImport", "H�mta data fr�n Eventor", CompetitionCB).setDefault();
      gdi.fillDown();
      gdi.popX();
    }
    else if (bi.id == "EventorImport") {
      const int diffZeroTime = 3600;
      DWORD id;
      DWORD db;
      gdi.getData("EventorId", id);
      gdi.getData("UpdateDB", db);

      DWORD eventIndex;
      gdi.getData("EventIndex", eventIndex);
      const CompetitionInfo *ci = 0;
      if (eventIndex < events.size())
        ci = &events[eventIndex];

      bool removeRemoved = true;
      if (gdi.hasField("RemoveRemoved"))
        removeRemoved = gdi.isChecked("RemoveRemoved");

      string course = gdi.getText("FileName", true);
      int startType = 0;
      const bool createNew = oe->getExtIdentifier() != id && id>0;
      int zeroTime = 0;
      int firstStart = 0;
      string lastEntry;
      if (id > 0 && createNew) {
        string fs = gdi.getText("FirstStart");
        int t = oEvent::convertAbsoluteTime(fs);
        if (t<0) {
          string msg = "Ogiltig starttid: X#" + fs;
          throw std::exception(msg.c_str());
        }
        firstStart = t;
        zeroTime = t - diffZeroTime;
        if (zeroTime<0)
          zeroTime += 3600*24;

        startType = gdi.getSelectedItem("StartType").first;
        lastEntry = gdi.getText("LastEntryDate");
      }

      if (gdi.hasField("EventorImport")) {
        gdi.disableInput("EventorImport");
        gdi.disableInput("FileName");
        gdi.disableInput("FirstStart");
      }

      string tEvent = getTempFile();
      string tClubs = getTempFile();
      string tClass = getTempFile();
      string tEntry = getTempFile();
      string tRunnerDB = db!= 0 ? getTempFile() : "";
      gdi.dropLine(3);
      try {
        getEventorCmpData(gdi, id, tEvent, tClubs, tClass, tEntry, tRunnerDB);
      }
      catch (std::exception &ex) {
        gdi.popX();
        gdi.dropLine();
        gdi.fillDown();
        gdi.addString("", 0, string("Fel: X#") + ex.what()).setColor(colorRed);
        gdi.addButton("Cancel", "�terg�", CompetitionCB);
        gdi.refresh();
        return 0;
      }

      gdi.fillDown();
      gdi.dropLine();

      if (db != 0) {
        gdi.addString("", 1, "Behandlar l�pardatabasen").setColor(colorGreen);
        vector<string> extractedFiles;
        gdi.fillRight();
        gdi.addString("", 0 , "Packar upp l�pardatabas...");
        gdi.refreshFast();
        unzip(tRunnerDB.c_str(), 0, extractedFiles);
        gdi.addString("", 0 , "OK");
        gdi.refreshFast();
        gdi.dropLine();
        gdi.popX();
        gdi.fillDown();
        removeTempFile(tRunnerDB);
        if (extractedFiles.size() != 1) {
          gdi.addString("", 0, "Unexpected file contents: X#" + tRunnerDB).setColor(colorRed);
        }
        if (extractedFiles.empty())
          tRunnerDB.clear();
        else
          tRunnerDB = extractedFiles[0];
      }

      oe->importXML_IOF_Data(tClubs.c_str(), tRunnerDB.c_str(), false);
      removeTempFile(tClubs);

      if (id > 0) {
        gdi.dropLine();
        gdi.addString("", 1, "Behandlar t�vlingsdata").setColor(colorGreen);

        if (createNew && id>0) {
          gdi.addString("", 1, "Skapar ny t�vling");
          oe->newCompetition("New");

          oe->importXML_EntryData(gdi, tEvent.c_str(), false, false);
          oe->setZeroTime(formatTimeHMS(zeroTime));
          oe->getDI().setDate("OrdinaryEntry", lastEntry);
          if (ci) {
            if (!ci->account.empty())
              oe->getDI().setString("Account", ci->account);

            if (!ci->url.empty())
              oe->getDI().setString("Homepage", ci->url);
          }
        }
        removeTempFile(tEvent);

        oe->importXML_EntryData(gdi, tClass.c_str(), false, false);
        removeTempFile(tClass);

        oe->importXML_EntryData(gdi, tEntry.c_str(), false, removeRemoved);
        removeTempFile(tEntry);

        if (!course.empty()) {
          gdi.dropLine();
          TabCourse::runCourseImport(gdi, course, oe, true);
        }

        bool drawn = false;
        if (createNew && startType>0) {
          gdi.scrollToBottom();
          gdi.dropLine();

          switch (startType) {
            case SMCommon:
              oe->automaticDrawAll(gdi, formatTimeHMS(firstStart), "0", "0", false, false, 1);
              drawn = true;
              break;

            case SMDrawn:
              ClassConfigInfo cnf;
              oe->getClassConfigurationInfo(cnf);
              bool skip = false;
              if (!cnf.classWithoutCourse.empty()) {
                string cls = "";
                for (size_t k = 0; k < cnf.classWithoutCourse.size(); k++) {
                  if (k>=5) {
                    cls += "...";
                    break;
                  }
                  if (k>0)
                    cls += ", ";
                  cls += cnf.classWithoutCourse[k];
                }
                if (!gdi.ask("ask:missingcourse#" + cls)) {
                  gdi.addString("", 0, "Skipper lottning");
                  skip = true;
                }
              }
              if (!skip)  
                oe->automaticDrawAll(gdi, formatTimeHMS(firstStart), "2:00", "2", true, true, 1);
              drawn = true;
              break;
          }
        }
      }

      gdi.dropLine();
      gdi.addString("", 1, "Klart").setColor(colorGreen);

      oe->getMeOSFeatures().useFeature(MeOSFeatures::Speaker, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::Economy, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::EditClub, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::Network, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::Vacancy, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::InForest, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::DrawStartList, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::Bib, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::RunnerDb, true, *oe);
      oe->getMeOSFeatures().useFeature(MeOSFeatures::ForkedIndividual, true, *oe);

      if (oe->hasTeam()) {
        oe->getMeOSFeatures().useFeature(MeOSFeatures::Relay, true, *oe);
      }

      gdi.scrollToBottom();
      gdi.dropLine();
      gdi.disableInput("Cancel"); // Disable "cancel" above
      gdi.fillRight();
      if (id > 0)
        gdi.addButton("StartIndividual", "Visa startlistan", ListsCB);
      gdi.addButton("Cancel", "�terg�", CompetitionCB);
      gdi.refreshFast();

    }
    else if (bi.id == "Cancel"){
      loadPage(gdi);
    }
    else if (bi.id == "WelcomeOK") {
      gdi.scaleSize(1.0/gdi.getScale());
      oe->setProperty("FirstTime", 0);
      loadPage(gdi);
    }
    else if (bi.id == "dbtest") {

    }
    else if (bi.id=="FreeImport") {
      gdi.clearPage(true);
      gdi.addString("", 2, "Fri anm�lningsimport");
      gdi.addString("", 10, "help:33940");
      gdi.dropLine(0.5);
      gdi.addInputBox("EntryText", 550, 280, entryText, 0, "");
      gdi.dropLine(0.5);
      gdi.fillRight();
      gdi.addButton("PreviewImport", "Granska inmatning", CompetitionCB, "tooltip:analyze");
      gdi.addButton("Cancel", "Avbryt", CompetitionCB);
      gdi.addButton("Paste", "Klistra in", CompetitionCB, "tooltip:paste");
      gdi.addButton("ImportFile", "Importera fil...", CompetitionCB, "tooltip:import");
      gdi.addButton("ImportDB", "Bygg databaser...", CompetitionCB, "tooltip:builddata");
      gdi.fillDown();
    }
    else if (bi.id=="ImportDB") {
      if (!gdi.ask("help:146122"))
        return 0;
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("xml-data", "*.xml;*.meos"));
      string file = gdi.browseForOpen(ext, "xml");

      if (file.empty())
        return 0;
      gdi.setWaitCursor(true);
      oe->getFreeImporter(fi);
      string info;

      oe->importXMLNames(file.c_str(), fi, info);
      fi.save();
      gdi.alert(info);
      gdi.setWaitCursor(false);
    }
    else if (bi.id=="Paste") {
      gdi.pasteText("EntryText");
    }
    else if (bi.id=="ImportFile") {
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("Textfiler", "*.txt"));

      string file=gdi.browseForOpen(ext, "txt");
      ifstream fin(file.c_str());
      char bf[1024];
      bf[0]='\r';
      bf[1]='\n';
      entryText.clear();
      while (fin.good() && !fin.eof()) {
        fin.getline(bf+2, 1024-2);
        entryText+=bf;
      }
      entryText+="\r\n";
      fin.close();

      gdi.setText("EntryText", entryText);
    }
    else if (bi.id=="PreviewImport") {
      oe->getFreeImporter(fi);
      entryText=gdi.getText("EntryText");
      gdi.clearPage(false);
      gdi.addString("", 2, "F�rhandsgranskning, import");
      gdi.dropLine(0.5);
      char *bf=new char[entryText.length()+1];
      strcpy_s(bf, entryText.length()+1, entryText.c_str());
      fi.extractEntries(bf, entries);
      delete[] bf;
      vector<pClass> cls;
      oe->getClasses(cls, true);
      for (size_t k = 0; k < entries.size(); k++) {
        if (entries[k].eClass.empty()) {
          if (!cls.empty()) {
            entries[k].eClass = cls.back()->getName(); // Fallback
          }
          else {
            entries[k].eClass = lang.tl("Klass");
          }
        }
      }
  
      fi.showEntries(gdi, entries);
      gdi.fillRight();
      gdi.dropLine(1);
      gdi.addButton("DoFreeImport", "Spara anm�lningar", CompetitionCB);
      gdi.addButton("FreeImport", "�ndra", CompetitionCB);

      gdi.addButton("Cancel", "Avbryt", CompetitionCB);
      gdi.scrollToBottom();
    }
    else if (bi.id=="DoFreeImport") {
      fi.addEntries(oe, entries);
      entryText.clear();
      loadPage(gdi);
    }
    else if (bi.id=="Startlist") {
      save(gdi, false);
      oe->sanityCheck(gdi, false);
      selectStartlistOptions(gdi);
    }
    else if (bi.id=="BrowseExport" || bi.id=="BrowseExportResult") {
      int FilterIndex = gdi.getSelectedItem("Type").first;
      vector< pair<string, string> > ext;
      if (bi.id=="BrowseExport") {
        ext.push_back(make_pair("IOF Startlista, version 3.0 (xml)", "*.xml"));
        ext.push_back(make_pair("IOF Startlista, version 2.0.3 (xml)", "*.xml"));
        ext.push_back(make_pair("OE Semikolonseparerad (csv)", "*.csv"));
        ext.push_back(make_pair("Webbdokument (html)", "*.html;*.htm"));
	  }
      else {
        ext.push_back(make_pair("IOF Resultat, version 3.0 (xml)", "*.xml"));
        ext.push_back(make_pair("IOF Resultat, version 2.0.3 (xml)", "*.xml"));
        ext.push_back(make_pair("OE Semikolonseparerad (csv)", "*.csv"));
        ext.push_back(make_pair("Webbdokument (html)", "*.html"));
	  }
      string save = gdi.browseForSave(ext, "xml", FilterIndex);

      if (save.length() > 0) {
        gdi.setText("Filename", save);
        gdi.selectItemByData("Type", FilterIndex);
        if (gdi.getExtra("Filename")) {
          gdi.enableInput((char *)gdi.getExtra("Filename"));
        }
      }
    }
    else if (bi.id=="DoSaveStartlist") {
      string save = gdi.getText("Filename");
      if (save.empty())
        throw meosException("Filnamn kan inte vara tomt");

      bool individual = !gdi.hasField("ExportTeam") || gdi.isChecked("ExportTeam");

      bool includeStage = true;
      if (gdi.hasField("IncludeRaceNumber"))
        includeStage = gdi.isChecked("IncludeRaceNumber");

      gdi.getSelection("ClassNewEntries", allTransfer);
      int FilterIndex = gdi.getSelectedItem("Type").first;
	  int cSVLanguageHeaderIndex = gdi.getSelectedItem("LanguageType").first;
	  bool useFFCOClubMapping = gdi.isChecked("UseFFCOClubMapping");

      gdi.setWaitCursor(true);

      if (FilterIndex == 1 || FilterIndex == 2) {
        bool useUTC = oe->getDCI().getInt("UTC") != 0;
        oe->exportIOFStartlist(FilterIndex == 1 ? oEvent::IOF30 : oEvent::IOF20,
                                save.c_str(), useUTC, allTransfer, individual, includeStage);
      }
      else if (FilterIndex == 3) {
        oe->exportOECSV(save.c_str(), cSVLanguageHeaderIndex, false, useFFCOClubMapping);
      }
	  else {
        oListParam par;
        par.listCode = EStdStartList;
        par.setLegNumberCoded(-1);
        oListInfo li;
        par.selection = allTransfer;
        oe->generateListInfo(par,  gdi.getLineHeight(), li);
        gdioutput tGdi("temp", gdi.getScale(), gdi.getEncoding());
        oe->generateList(tGdi, true, li, false);
        tGdi.writeTableHTML(gdi.toWide(save), oe->getName(), 0);
        tGdi.openDoc(save.c_str());
      }
      loadPage(gdi);
    }
    else if (bi.id=="Splits") {
      save(gdi, false);
      oe->sanityCheck(gdi, true);
      selectExportSplitOptions(gdi);
    }
    else if (bi.id == "DoSaveSplits") {
      string save = gdi.getText("Filename");
      if (save.empty())
        throw meosException("Filnamn kan inte vara tomt");

      //bool individual = !gdi.hasField("ExportTeam") || gdi.isChecked("ExportTeam");
      gdi.getSelection("ClassNewEntries", allTransfer);
      
      int FilterIndex = gdi.getSelectedItem("Type").first;
	  int cSVLanguageHeaderIndex = gdi.getSelectedItem("LanguageType").first;
	  bool includeSplits = gdi.isChecked("ExportSplitTimes");
	  bool useFFCOClubMapping = gdi.isChecked("UseFFCOClubMapping");

      bool unroll = gdi.isChecked("UnrollLoops"); // If not applicable, field does not exist.
      bool includeStage = true;
      if (gdi.hasField("IncludeRaceNumber"))
        includeStage = gdi.isChecked("IncludeRaceNumber");

      gdi.setWaitCursor(true);
      if (FilterIndex == 1 || FilterIndex == 2) {
        oEvent::IOFVersion ver = FilterIndex == 1 ? oEvent::IOF30 : oEvent::IOF20;
        ClassConfigInfo cnf;
        oe->getClassConfigurationInfo(cnf);
        bool useUTC = oe->getDCI().getInt("UTC") != 0;

        if (!cnf.hasTeamClass()) {
          oe->exportIOFSplits(ver, save.c_str(), true, useUTC, 
                              allTransfer, -1, false, unroll, includeStage);
        }
        else {
          ListBoxInfo leglbi;
          gdi.getSelectedItem("LegType", leglbi);
          string file = save;
          if (leglbi.data == 2) {
            string fileBase;
            string fileEnd = file.substr(file.length()-4);
            if (_stricmp(fileEnd.c_str(), ".XML") == 0)
              fileBase = file.substr(0, file.length() - 4);
            else {
              fileEnd = ".xml";
              fileBase = file;
            }
            ClassConfigInfo cnf;
            oe->getClassConfigurationInfo(cnf);
            int legMax = cnf.getNumLegsTotal();
            for (int leg = 0; leg<legMax; leg++) {
              file = fileBase + "_" + itos(leg+1) + fileEnd;
              oe->exportIOFSplits(ver, file.c_str(), true, useUTC, 
                                  allTransfer, leg, false, unroll, includeStage);
            }
          }
          else if (leglbi.data == 3) {
            oe->exportIOFSplits(ver, file.c_str(), true, useUTC, allTransfer, 
                                -1, true, unroll, includeStage);
          }
          else {
            int leg = leglbi.data == 1 ? -1 : leglbi.data - 10;
            oe->exportIOFSplits(ver, file.c_str(), true, useUTC, allTransfer, 
                                leg, false, unroll, includeStage);
          }
        }
      }
      else if (FilterIndex == 3) {
        oe->exportOECSV(save.c_str(), cSVLanguageHeaderIndex, includeSplits, useFFCOClubMapping);
      }
	  else {
        oListParam par;
        par.listCode = EStdResultList;
        par.showSplitTimes = true;
        par.setLegNumberCoded(-1);
        oListInfo li;
        oe->generateListInfo(par,  gdi.getLineHeight(), li);
        gdioutput tGdi("temp", gdi.getScale(), gdi.getEncoding());
        oe->generateList(tGdi, true, li, false);
        tGdi.writeTableHTML(gdi.toWide(save), oe->getName(), 0);
        tGdi.openDoc(save.c_str());
      }

      loadPage(gdi);
    }
    else if (bi.id=="SaveAs") {
      oe->sanityCheck(gdi, false);
      save(gdi, true);
      exportFileAs(hWndMain, gdi);
    }
    else if (bi.id=="Duplicate") {
      oe->duplicate();
      gdi.alert("Skapade en lokal kopia av t�vlingen.");
    }
    else if (bi.id=="Import") {
      //Import complete competition
      importFile(hWndMain, gdi);
      loadPage(gdi);
    }
    else if (bi.id=="Restore") {
      listBackups(gdi);
    }
    else if (bi.id=="Save") {
      save(gdi, true);
      resetSaveTimer();
    }
    else if (bi.id=="CloseCmp") {
      gdi.setWaitCursor(true);
      if (!showConnectionPage)
        save(gdi, false);
      oe->save();
      oe->newCompetition("");
      resetSaveTimer();
      gdi.setWindowTitle("");
      if (showConnectionPage)
        loadConnectionPage(gdi);
      else
        loadPage(gdi);
      gdi.setWaitCursor(false);
    }
    else if (bi.id=="Delete" &&
      gdi.ask("Vill du verkligen radera t�vlingen?")) {

      if (oe->isClient())
        oe->dropDatabase();
      else if (!oe->deleteCompetition())
        gdi.alert("Operation failed. It is not possible to delete competitions on server");

      oe->clearListedCmp();
      oe->newCompetition("");
      gdi.setWindowTitle("");
      loadPage(gdi);
    }
    else if (bi.id=="NewCmp") {
      bool guideMode = true;
      if (guideMode) {
        newCompetitionGuide(gdi, 0);
        return 0;
      }

      oe->newCompetition(lang.tl("Ny t�vling"));
      gdi.setWindowTitle("");

      if (useEventor()) {
        int age = getRelativeDay() - oe->getPropertyInt("DatabaseUpdate", 0);
        if (age>60 && gdi.ask("help:dbage")) {
          bi.id = "EventorUpdateDB";
          if (checkEventor(gdi, bi))
            return 0;
          return competitionCB(gdi, type, &bi);
        }
      }

      loadPage(gdi);
      return 0;
    }
    else if (bi.id=="OpenCmp") {
      ListBoxInfo lbi;
      int id=0;
      bool frontPage=true;
      if (!gdi.getSelectedItem("CmpSel", lbi)) {
        frontPage=false;
        if ( gdi.getSelectedItem("ServerCmp", lbi) )
          id=lbi.data;
        else if ( gdi.getSelectedItem("LocalCmp", lbi) )
          id=lbi.data;
      }
      else id=lbi.data;

      if (id==0)
        throw meosException("Ingen t�vling vald.");

      openCompetition(gdi, id);
     
      if (frontPage)
        loadPage(gdi);
      else {
        oe->setProperty("UseDirectSocket", gdi.isChecked("UseDirectSocket"));
        oe->verifyConnection();
        oe->validateClients();
        loadConnectionPage(gdi);
      }

      if (oe->isClient() && oe->getPropertyInt("UseDirectSocket", true) != 0) {
          oe->getDirectSocket().startUDPSocketThread(gdi.getMain());
      }
      return 0;
    }
    else if (bi.id=="BrowseCourse") {
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("Banor, OCAD semikolonseparerat", "*.csv;*.txt"));
      ext.push_back(make_pair("Banor, IOF (xml)", "*.xml"));

      string file = gdi.browseForOpen(ext, "csv");
      if (file.length()>0)
        gdi.setText("FileName", file);
    }
    else if (bi.id=="BrowseEntries") {
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("Importerbara", "*.xml;*.csv"));
      ext.push_back(make_pair("IOF (xml)", "*.xml"));
      ext.push_back(make_pair("OE Semikolonseparerad (csv)", "*.csv"));

      string file = gdi.browseForOpen(ext, "xml");
      if (file.length()>0) {
        const char *ctrl = bi.getExtra();
        if (ctrl != 0)
          gdi.setText(ctrl, file);
      }
    }
    else if (bi.id=="Entries") {
      if (!save(gdi, false))
        return 0;

      gdi.clearPage(false);

      entryForm(gdi, false);

      gdi.pushX();
      gdi.fillRight();
      gdi.addButton("DoImport", "Importera", CompetitionCB);
      gdi.fillDown();
      gdi.addButton("Cancel", "Avbryt", CompetitionCB);
      gdi.popX();
      gdi.refresh();
    }
    else if (bi.id=="DoImport") {
      gdi.enableEditControls(false);
      gdi.disableInput("DoImport");
      gdi.disableInput("Cancel");
      gdi.disableInput("BrowseEntries");
      bool removeRemoved = gdi.isChecked("RemoveRemoved");
	  bool reverseNames = gdi.isChecked("ReverseNames");
	  bool useFFCOClubMapping = gdi.isChecked("UseFFCOClubMapping");

	  try {
        gdi.autoRefresh(true);
        saveEntries(gdi, removeRemoved, false, reverseNames, useFFCOClubMapping);
      }
      catch (std::exception &) {
        gdi.enableEditControls(true);
        gdi.enableInput("DoImport");
        gdi.enableInput("Cancel");
        gdi.enableInput("BrowseEntries");
        gdi.refresh();
        throw;
      }

      gdi.addButton("Cancel", "OK", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id=="Courses") {
      if (!save(gdi, false))
        return 0;
      TabCourse::setupCourseImport(gdi, CompetitionCB);
    }
    else if (bi.id=="DoImportCourse") {
      string filename = gdi.getText("FileName");
      if (filename.empty())
        return 0;
      gdi.disableInput("DoImportCourse");
      gdi.disableInput("Cancel");
      gdi.disableInput("BrowseCourse");
      gdi.disableInput("AddClasses");

      try {
        TabCourse::runCourseImport(gdi, filename, oe, gdi.isChecked("AddClasses"));
      }
      catch (std::exception &) {
        gdi.enableInput("DoImportCourse");
        gdi.enableInput("Cancel");
        gdi.enableInput("BrowseCourse");
        gdi.enableInput("AddClasses");
        throw;
      }
      gdi.dropLine();
      gdi.addButton("Cancel", "OK", CompetitionCB);
      gdi.refresh();
    }
    else if (bi.id=="About") {
      loadAboutPage(gdi);
    }
    else if (bi.id == "DBEntry") {
      int classId = gdi.getSelectedItem("Classes").first;

      DWORD data;
      gdi.getData("RunnerIx", data);
      RunnerDBEntry *dbr = oe->getRunnerDatabase().getRunnerByIndex(data);

      // Construct runner from database
      oRunner sRunner(oe, 0);
      sRunner.init(*dbr);
      oe->addRunnerFromDB(&sRunner, classId, true);
      loadRunnerDB(gdi, 1, false);
    }
    else if (bi.id == "CancelRunnerDatabase") {
      if (!oe->empty())
        loadRunnerDB(gdi, 0, false);
      else
        loadPage(gdi);
    }
    else if (bi.id == "CancelEntry") {
      loadRunnerDB(gdi, 1, false);
    }
    else if (bi.id == "RunnerDB") {
      loadRunnerDB(gdi, 1, true);
    }
    else if (bi.id == "ClubDB") {
      loadRunnerDB(gdi, 2, true);
    }
    else if (bi.id == "ExportRunnerDB") {
      xmlparser xml(gdi.getEncoding() == ANSI ? 0 : &gdi);
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("IOF L�pardatabas, version 3.0 (xml)", "*.xml"));
      int ix;
      string fileName = gdi.browseForSave(ext, "xml", ix);
      if (fileName.empty())
        return false;

      gdi.setWaitCursor(true);
      xml.openOutput(fileName.c_str(), false);
      IOF30Interface writer(oe);
      writer.writeRunnerDB(oe->getRunnerDatabase(), xml);
      gdi.setWaitCursor(false);
    }
    else if (bi.id == "ExportClubDB") {
      xmlparser xml(gdi.getEncoding() == ANSI ? 0 : &gdi);
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("IOF Klubbdatabas, version 3.0 (xml)", "*.xml"));
      int ix;
      string fileName = gdi.browseForSave(ext, "xml", ix);
      if (fileName.empty())
        return false;

      gdi.setWaitCursor(true);
      xml.openOutput(fileName.c_str(), false);
      IOF30Interface writer(oe);
      writer.writeClubDB(oe->getRunnerDatabase(), xml);
      gdi.setWaitCursor(false);
    }
    else if (bi.id == "ClearDB") {
      if (gdi.ask("ask:cleardb")) {
        oe->getRunnerDatabase().clearClubs();
        oe->saveRunnerDatabase("database", true);
        if (oe->isClient()) {
          msUploadRunnerDB(oe);
        }
        loadRunnerDB(gdi, 0, false);
      }
    }
  }
  else if (type==GUI_LISTBOXSELECT) {
    ListBoxInfo lbi=*(ListBoxInfo *)data;
    if (lbi.id == "LocalCmp") {
      gdi.selectItemByData("ServerCmp", -1);
      gdi.sendCtrlMessage("OpenCmp");
    }
    else if (lbi.id == "ServerCmp") {
      gdi.selectItemByData("LocalCmp", -1);
      gdi.sendCtrlMessage("OpenCmp");
    }
  }
  else if (type==GUI_LISTBOX) {
    ListBoxInfo lbi=*(ListBoxInfo *)data;

    if (lbi.id=="LocalCmp") {
      gdi.selectItemByData("ServerCmp", -1);
      gdi.disableInput("Repair", true);
    }
    else if (lbi.id=="ServerCmp") {
      gdi.selectItemByData("LocalCmp", -1);
      gdi.enableInput("Repair", true);
    }
    else if (lbi.id=="TextSize") {
      int textSize = lbi.data;
      oe->setProperty("TextSize", textSize);
      gdi.setFont(textSize, oe->getPropertyString("TextFont", "Arial"), interpetEncoding(lang.tl("encoding")));
      PostMessage(gdi.getTarget(), WM_USER + 2, TCmpTab, 0);
    }
    else if (lbi.id == "Language") {
      lang.get().loadLangResource(lbi.text);
      oe->updateTabs(true);
      oe->setProperty("Language", lbi.text);
      //gdi.setEncoding(interpetEncoding(lang.tl("encoding")));
      gdi.setFont(oe->getPropertyInt("TextSize", 0), oe->getPropertyString("TextFont", "Arial"), interpetEncoding(lang.tl("encoding")));
      PostMessage(gdi.getTarget(), WM_USER + 2, TCmpTab, 0);
    }
    else if (lbi.id == "PreEvent") {
      gdi.setInputStatus("OpenPre", int(lbi.data)>0);
    }
    else if (lbi.id == "PostEvent") {
      bool hasPost = int(lbi.data)>0;
      gdi.setInputStatus("OpenPost", hasPost);
      gdi.setInputStatus("TransferData", hasPost);
      gdi.setInputStatus("CloneCmp", !hasPost);
    }
    else if (lbi.id == "StageNumber") {
      int nr = int(lbi.data);
      oe->setStageNumber(nr);
      oe->synchronize(true);
    }
    else if (lbi.id == "Type") {
      if (gdi.hasField("LegType")) {
        gdi.setInputStatus("LegType", lbi.data == 1 || lbi.data == 2); // Enable on IOF-XML
      }
      else if (gdi.hasField("ExportTeam")) {
        gdi.setInputStatus("ExportTeam", lbi.data == 1); // Enable on IOF-XML
      }
	  else if (gdi.hasField("LanguageType")) { // Enable / Disable options for OE CSV export of results
		  if (gdi.getSelectedItem("Type").first == 3) {
			  // OE CSV
			  gdi.enableInput("LanguageType");
			  if (gdi.hasField("ExportSplitTimes")) {
				  gdi.enableInput("ExportSplitTimes");
			  }
			  gdi.enableInput("UseFFCOClubMapping");
		  }
		  else {
			  gdi.disableInput("LanguageType");
			  if (gdi.hasField("ExportSplitTimes")) {
				  gdi.disableInput("ExportSplitTimes");
			  }
			  gdi.disableInput("UseFFCOClubMapping");
		  }
	  }
    }
  }
  else if (type== GUI_INPUT) {
    InputInfo ii=*(InputInfo *)data;
    if (ii.id == "Filename") {
      if (ii.getExtra()) {
        gdi.setInputStatus((char *)ii.getExtra(), !ii.text.empty());
      }
    }
    else if (ii.id == "NumStages") {
      int ns = gdi.getTextNo("NumStages");
      oe->setNumStages(ns);
      oe->synchronize(true);
    }
  }
  else if (type == GUI_INPUTCHANGE) {
    InputInfo ii=*(InputInfo *)data;
    if (ii.id == "Filename") {
      if (ii.getExtra()) {
        gdi.setInputStatus((char *)ii.getExtra(), !ii.text.empty());
      }
    }
  }
  else if (type==GUI_EVENT) {
    EventInfo ei=*(EventInfo *)data;

    if ( ei.id=="Connections" ) {
      string s=gdi.getText("ClientName");
      loadConnectionPage(gdi);
      gdi.setText("ClientName", s);
    }
    else if (ei.id=="CellAction") {
      string org = ei.getOrigin();
      if (org == "runnerdb") {
        int ix = ei.getExtraInt();
        const RunnerDBEntry *pRdb = oe->getRunnerDatabase().getRunnerByIndex(ix);
        if (pRdb == 0)
          throw meosException("Internal error");

        const RunnerDBEntry &rdb = *pRdb;
        vector<int> classes;
        bool suggest = oe->getClassesFromBirthYear(rdb.getBirthYear(), interpretSex(rdb.getSex()), classes);

        gdi.clearPage(true, false);
        if (suggest || find(classes.begin(), classes.end(), lastSelectedClass) == classes.end()) {
          if (classes.empty())
            lastSelectedClass = -1;
          else
            lastSelectedClass = classes.back();
        }

        string name;
        rdb.getName(name);
        gdi.addString("", boldLarge, "Anm�l X#" + name);
        gdi.setData("RunnerIx", ix);
        gdi.dropLine();
        gdi.addSelection("Classes", 200, 300, 0, "Klasser:");
        oe->fillClasses(gdi, "Classes", oEvent::extraNone, oEvent::filterNone);

        if (lastSelectedClass != -1)
          gdi.selectItemByData("Classes", lastSelectedClass);
        else
          gdi.selectFirstItem("Classes");

        gdi.dropLine();
        gdi.fillRight();
        gdi.addButton("DBEntry", "Anm�l", CompetitionCB).setDefault();
        gdi.addButton("CancelEntry", "Avbryt", CompetitionCB).setCancel();
        gdi.refresh();
      }
    }
  }
  else if (type==GUI_CLEAR) {
    if (gdi.isInputChanged("")) {
      if (gdi.hasField("SaveSettings")) {
        gdi.sendCtrlMessage("SaveSettings");
      }
      else {
        string name=gdi.getText("Name");

        if (!name.empty() && !oe->empty())
          save(gdi, false);
      }
    }
    return 1;
  }
  return 0;
}

void TabCompetition::openCompetition(gdioutput &gdi, int id) {
  gdi.setWaitCursor(true);
  string err;
  try {
    if (!oe->open(id)) {
      gdi.alert("Kunde inte �ppna t�vlingen.");
      return;
    }
  }
  catch (const meosException &ex) {
    err = ex.what();
  }

  if (gSI) gSI->SetZeroTime(oe->getZeroTimeNum());
  resetSaveTimer();
  oe->setProperty("LastCompetition", id);
  gdi.setWindowTitle(oe->getTitleName());
  oe->updateTabs();

  if (!err.empty()) {
    gdi.alert(err);
  }
}

int TabCompetition::restoreCB(gdioutput &gdi, int type, void *data) {
  TextInfo &ti = *(TextInfo *)data;
  int id = ti.getExtraInt();
  const BackupInfo &bi = oe->getBackup(id);

  if (ti.id == "") {
    string fi(bi.FullPath);
    if (!oe->open(fi.c_str(), false)) {
      gdi.alert("Kunde inte �ppna t�vlingen.");
    }
    else {
      if (gSI) gSI->SetZeroTime(oe->getZeroTimeNum());

      const string &name = oe->getName();
      if (name.find_last_of("}") != name.length()-1)
        oe->setName(name + " {" + lang.tl("�terst�lld") +"}");

      oe->restoreBackup();

      gdi.setWindowTitle(oe->getTitleName());
      oe->updateTabs();
      resetSaveTimer();
      loadPage(gdi);
    }
  }
  else if (ti.id == "EraseBackup") {
    if (gdi.ask("Vill du ta bort alla s�kerhetskopior p� X?#" + bi.Name)) {
      gdi.setWaitCursor(true);
      oe->deleteBackups(bi);
      listBackups(gdi);
    }
  }
  return 0;
}

void TabCompetition::listBackups(gdioutput &gdi) {
  char bf[260];
  getUserFile(bf, "");
  int yo = gdi.GetOffsetY();
  gdi.clearPage(false);
  oe->enumerateBackups(bf);

  gdi.addString("", boldLarge|Capitalize, "Lagrade s�kerhetskopior");
  gdi.addString("", 0, "help:restore_backup");
  gdi.dropLine(0.4);
  gdi.addButton("Cancel", "�terg�", CompetitionCB);
  gdi.dropLine();
  oe->listBackups(gdi, ::restoreCB);
  gdi.scrollTo(0, yo);
  gdi.refresh();
}

void TabCompetition::copyrightLine(gdioutput &gdi) const
{
  gdi.pushX();
  gdi.fillRight();

  gdi.addButton("Help", "Hj�lp", CompetitionCB, "");
  gdi.addButton("About", "Om MeOS...", CompetitionCB);

  gdi.dropLine(0.4);
  gdi.fillDown();
  gdi.addString("", 0, MakeDash("#Copyright � 2007-2016 Melin Software HB"));
  gdi.dropLine(1);
  gdi.popX();

  gdi.addString("", 0, getMeosFullVersion()).setColor(colorDarkRed);
  gdi.dropLine(0.2);
}

void TabCompetition::loadAboutPage(gdioutput &gdi) const
{
  gdi.clearPage(false);
  gdi.addString("", 2, MakeDash("Om MeOS - ett Mycket Enkelt OrienteringsSystem")).setColor(colorDarkBlue);
  gdi.dropLine(2);
  gdi.addStringUT(1, MakeDash("Copyright � 2007-2016 Melin Software HB"));
  gdi.dropLine();
  gdi.addStringUT(10, "The database connection used is MySQL++\nCopyright "
                        "(c) 1998 by Kevin Atkinson, (c) 1999, 2000 and 2001 by MySQL AB,"
                        "\nand (c) 2004-2007 by Educational Technology Resources, Inc.\n"
                        "The database used is MySQL, Copyright (c) 2008-2016 Oracle, Inc."
                        "\n\nGerman Translation by Erik Nilsson-Simkovics"
                        "\n\nDanish Translation by Michael Leth Jess and Chris Bagge"
                        "\n\nRussian Translation by Paul A. Kazakov and Albert Salihov"
                        "\n\nFrench Translation by Jerome Monclard");

  gdi.dropLine();
  gdi.addString("", 0, "Det h�r programmet levereras utan n�gon som helst garanti. Programmet �r ");
  gdi.addString("", 0, "fritt att anv�nda och du �r v�lkommen att distribuera det under vissa villkor,");
  gdi.addString("", 0, "se license.txt som levereras med programmet.");

  gdi.dropLine();
  gdi.addString("", 1, "Vi st�der MeOS");
  vector<string> supp;
  getSupporters(supp);
  for (size_t k = 0; k<supp.size(); k++)
    gdi.addStringUT(0, supp[k]);

  gdi.dropLine();
  gdi.addButton("Cancel", "St�ng", CompetitionCB);
  gdi.refresh();
}

bool TabCompetition::useEventor() const {
  return oe->getPropertyInt("UseEventor", 0) == 1;
}

bool TabCompetition::loadPage(gdioutput &gdi)
{
  if (oe->getPropertyInt("FirstTime", 1) == 1) {
    welcomeToMeOS(gdi);
    return true;
  }
  showConnectionPage=false;
  oe->checkDB();
  gdi.clearPage(true);
  gdi.fillDown();

  if (oe->empty()) {
    gdi.addString("", 2, "V�lkommen till MeOS");
    gdi.addString("", 1, MakeDash("#- ")+ lang.tl("ett Mycket Enkelt OrienteringsSystem")).setColor(colorDarkBlue);
    gdi.dropLine();

    if (oe->getPropertyInt("UseEventor", 0) == 0) {
      if ( gdi.ask("eventor:question#" + lang.tl("eventor:help")) )
        oe->setProperty("UseEventor", 1);
      else
        oe->setProperty("UseEventor", 2);
    }

    gdi.fillRight();
    gdi.pushX();

    gdi.addSelection("CmpSel", 300, 400, CompetitionCB, "V�lj t�vling:");

    char bf[260];
    getUserFile(bf, "");
    oe->enumerateCompetitions(bf, "*.meos");
    oe->fillCompetitions(gdi, "CmpSel",0);
    gdi.autoGrow("CmpSel");
    gdi.selectFirstItem("CmpSel");

    int lastCmp = oe->getPropertyInt("LastCompetition", 0);
    gdi.selectItemByData("CmpSel", lastCmp);

    gdi.dropLine();
    gdi.addButton("OpenCmp", "�ppna", CompetitionCB, "�ppna vald t�vling").setDefault();

    gdi.dropLine(4);
    gdi.popX();

    gdi.addButton("NewCmp", "Ny t�vling", CompetitionCB, "Skapa en ny, tom, t�vling");
    if (useEventor())
      gdi.addButton("Eventor", "T�vling fr�n Eventor...", CompetitionCB, "Skapa en ny t�vling med data fr�n Eventor");
    gdi.addButton("ConnectMySQL", "Databasanslutning...", CompetitionCB, "Anslut till en server");

    gdi.popX();
    gdi.dropLine(2.5);
    gdi.addButton("Import", "Importera t�vling...", CompetitionCB, "Importera en t�vling fr�n fil");
    gdi.addButton("Restore", "�terst�ll s�kerhetskopia...", CompetitionCB, "Visa tillg�ngliga s�kerhetskopior");
    gdi.addButton("LocalSettings", "�ndra lokala inst�llningar...", CompetitionCB);

    gdi.popX();
    gdi.dropLine(3);

    gdi.dropLine(2.3);
    textSizeControl(gdi);

    gdi.popX();
    gdi.dropLine(3);
    if (enableTests) {
      gdi.fillRight();
      gdi.addButton("SaveTest", "#Save test", CompetitionCB);
      gdi.addButton("RunTest", "#Run tests", CompetitionCB);
      gdi.addSelection("Tests", 200, 200, 0);
      vector< pair<string, size_t> > tests;
      TestMeOS tm(oe, "ALL");
      tm.getTests(tests);
      gdi.addItem("Tests", tests);
      gdi.selectFirstItem("Tests");
      gdi.addButton("RunSpecificTest", "#Run", CompetitionCB);
      
      gdi.dropLine(2);
      gdi.popX();
    }

    gdi.fillDown();
    
    copyrightLine(gdi);

    gdi.addButton(gdi.GetPageX()-gdi.scaleLength(180),
                  gdi.getCY()-gdi.getButtonHeight(),
                  "Exit", "Avsluta", CompetitionCB);
    gdi.setInputFocus("CmpSel", true);
  }
  else {
    oe->checkNecessaryFeatures();
    gdi.selectTab(tabId);

    gdi.addString("", 3, "MeOS");
    gdi.dropLine();
    oe->synchronize();

    gdi.pushX();
    gdi.fillRight();
    gdi.addInput("Name", oe->getName(), 24, 0, "T�vlingsnamn:");
    gdi.fillDown();

    gdi.addInput("Annotation", oe->getAnnotation(), 20, 0, "Kommentar / version:")
       .setBgColor(colorLightCyan);
    gdi.popX();

    gdi.fillRight();
    gdi.addInput("Date", oe->getDate(), 8, 0, "Datum:");
    gdi.addInput("ZeroTime", oe->getZeroTime(), 8, 0, "Nolltid:");

    gdi.fillDown();
    gdi.dropLine(1.2);
    gdi.addCheckbox("LongTimes", "Aktivera st�d f�r tider �ver 24 timmar", CompetitionCB, oe->useLongTimes());

    if (oe->isClient()) {
      gdi.popX();
      gdi.disableInput("ZeroTime");
      gdi.disableInput("LongTimes");
      if (oe->useLongTimes())
        gdi.disableInput("Date");
    }
    else {
      gdi.popX();
      if (!oe->useLongTimes())
        gdi.addString("ZeroTimeHelp", 0, "help:zero_time");
      else {
        gdi.addString("ZeroTimeHelp", 0, "help:long_times");
        gdi.disableInput("ZeroTime");
      }
    }

    gdi.fillRight();
    gdi.dropLine();

    if (oe->getExtIdentifier() > 0 && useEventor()) {
      gdi.addButton("SynchEventor", "Eventorkoppling", CompetitionCB, "Utbyt t�vlingsdata med Eventor");
    }

    gdi.addButton("Settings", "T�vlingsinst�llningar", CompetitionCB);
    gdi.addButton("Report", "T�vlingsrapport", CompetitionCB);
    gdi.addButton("Features", "MeOS Funktioner", CompetitionCB);

#ifdef _DEBUG
    gdi.addButton("Test", "Test", CompetitionCB);
#endif

    gdi.fillDown();
    gdi.popX();

    gdi.dropLine(3);

    //gdi.fillRight();
    //gdi.addCheckbox("UseEconomy", "Hantera klubbar och ekonomi", CompetitionCB, oe->useEconomy());
    //gdi.addCheckbox("UseSpeaker", "Anv�nd speakerst�d", CompetitionCB, oe->getDCI().getInt("UseSpeaker")!=0);
    //gdi.popX();
    //gdi.dropLine(2);

    //gdi.addCheckbox("UseRunnerDb", "Anv�nd l�pardatabasen", CompetitionCB, oe->useRunnerDb());

    //gdi.popX();
    //gdi.dropLine(2);
    textSizeControl(gdi);

    gdi.dropLine(4);
    gdi.popX();
    gdi.fillRight();
    gdi.addButton("Save", "Spara", CompetitionCB, "help:save");
    gdi.addButton("SaveAs", "Spara som fil...", CompetitionCB, "");
    gdi.addButton("Duplicate", "Duplicera", CompetitionCB, "help:duplicate");

    gdi.addButton("Delete", "Radera", CompetitionCB);
    gdi.addButton("CloseCmp", "St�ng", CompetitionCB);

    gdi.dropLine(2.5);
    gdi.popX();

#ifdef D_DEBUG
    gdi.dropLine(2.5);
    gdi.popX();
    gdi.addButton("CloneEvent", "#! Klona", CompetitionCB);
#endif

    gdi.fillDown();
    gdi.popX();

    gdi.newColumn();
    gdi.dropLine(3);
    gdi.setCX(gdi.getCX()+gdi.scaleLength(60));

    RECT rc;
    rc.top = gdi.getCY() - gdi.scaleLength(30);
    rc.left = gdi.getCX() - gdi.scaleLength(30);

    int bw = gdi.scaleLength(150);
    gdi.addString("", 1, "Importera t�vlingsdata");
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "Entries", "Anm�lningar",
                  CompetitionCB, "",  false, false);
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "FreeImport", "Fri anm�lningsimport",
                  CompetitionCB, "", false, false);
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "Courses", "Banor",
                  CompetitionCB, "", false, false);

    gdi.dropLine();
    gdi.addString("", 1, "Exportera t�vlingsdata");
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "Startlist", "Startlista",
                  CompetitionCB, "Exportera startlista p� fil", false, false);
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "Splits", "Resultat && str�cktider",
                  CompetitionCB, "Exportera resultat p� fil", false, false);

    gdi.dropLine();
    gdi.addString("", 1, "Funktioner");
    if (oe->useRunnerDb()) {
      gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "RunnerDatabase", "L�pardatabasen",
                    CompetitionCB, "Visa och hantera l�pardatabasen", false, false);
    }

    if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::SeveralStages)) {
      gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "MultiEvent", "Hantera flera etapper",
                    CompetitionCB, "", false, false);
    }
    gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "SaveAs", "S�kerhetskopiera",
                  CompetitionCB, "", false, false);
    if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Network)) {
      gdi.addButton(gdi.getCX(), gdi.getCY(), bw, "ConnectMySQL", "Databasanslutning",
                    CompetitionCB, "", false, false);
    }
    rc.bottom = gdi.getCY() + gdi.scaleLength(30);
    rc.right = rc.left + bw + gdi.scaleLength(60);

    gdi.addRectangle(rc, colorLightBlue);

    gdi.popX();

    gdi.dropLine(3);
    copyrightLine(gdi);

    gdi.setOnClearCb(CompetitionCB);
  }
  gdi.refresh();
  return true;
}

void TabCompetition::textSizeControl(gdioutput &gdi) const
{
  gdi.dropLine();
  int s = oe->getPropertyInt("TextSize", 0);
  const char *id="TextSize";
  gdi.fillRight();
  RECT rc;
  int x = gdi.getCX() + gdi.scaleLength(15);
  gdi.dropLine(-0.5);
  rc.top = gdi.getCY() - gdi.scaleLength(10);
  rc.left = gdi.getCX();

  gdi.setCX(x);

  gdi.addString("", 1, "Programinst�llningar");
  gdi.dropLine(2);
  gdi.setCX(x);
  //gdi.addString("", 0, "Textstorlek:");

  gdi.addSelection(id, 90, 200, CompetitionCB, "Textstorlek:");
  gdi.addItem(id, lang.tl("Normal"), 0);
  gdi.addItem(id, lang.tl("Stor"), 1);
  gdi.addItem(id, lang.tl("St�rre"), 2);
  gdi.addItem(id, lang.tl("St�rst"), 3);
  gdi.selectItemByData(id, s);

  id = "Language";
  gdi.addSelection(id, 150, 300, CompetitionCB, "Spr�k:");
  vector<string> ln = lang.get().getLangResource();
  string current = oe->getPropertyString("Language", "Svenska");
  int ix = -1;
  for (size_t k = 0; k<ln.size(); k++) {
    gdi.addItem(id, ln[k], k);
    if (ln[k] == current)
      ix = k;
  }
  gdi.selectItemByData(id, ix);

  if (oe->empty()) {
    gdi.setCX(gdi.getCX()+gdi.getLineHeight()*2);
    gdi.dropLine();
    gdi.addButton("Setup", "Inst�llningar...", CompetitionCB);

    rc.right = gdi.getCX() + gdi.scaleLength(15);

    gdi.setCX(x);
    gdi.dropLine(3);

    gdi.addCheckbox("UseEventor", "Anv�nd Eventor", CompetitionCB,
          useEventor(), "eventor:help");

    rc.bottom = gdi.getCY() + gdi.scaleLength(25);
  }
  else {
    rc.right = gdi.getWidth();//gdi.getCX() + gdi.scaleLength(10);
    rc.bottom = gdi.getCY() + gdi.scaleLength(50);
  }

  gdi.addRectangle(rc, colorLightYellow);
  gdi.dropLine();
}

int TabCompetition::getOrganizer(bool updateEvent) {
  string apikey = oe->getPropertyStringDecrypt("apikey", "");
  if (apikey.empty())
    return 0;
  if (!isAscii(apikey))
    return 0;

  Download dwl;
  dwl.initInternet();
  vector< pair<string, string> > key;
  string file = getTempFile();
  key.push_back(pair<string, string>("ApiKey", apikey));
  string url = eventorBase + "organisation/apiKey";
  try {
    dwl.downloadFile(url, file, key);
  }
  catch (dwException &ex) {
    if (ex.code == 403)
      return 0;
    else {
      throw std::exception("Kunde inte ansluta till Eventor.");
    }
  }
  catch (std::exception &) {
    throw std::exception("Kunde inte ansluta till Eventor.");
  }

  dwl.createDownloadThread();
  while (dwl.isWorking()) {
    Sleep(50);
  }

  int clubId = 0;

  xmlparser xml(0);
  xmlList xmlEvents;
  try {
    xml.read(file.c_str());
    xmlobject obj = xml.getObject("Organisation");
    if (obj) {
      clubId = obj.getObjectInt("OrganisationId");
      obj.getObjectString("Name", eventor.name);

      xmlobject ads = obj.getObject("Address");
      if (ads) {
        ads.getObjectString("careOf", eventor.careOf);
        ads.getObjectString("street", eventor.street);
        ads.getObjectString("city", eventor.city);
        ads.getObjectString("zipCode", eventor.zipCode);
      }

      xmlobject tele = obj.getObject("Tele");

      if (tele) {
        tele.getObjectString("mailAddress", eventor.email);
      }

      xmlobject aco = obj.getObject("Account");
      if (aco) {
        aco.getObjectString("AccountNo", eventor.account);
      }
    }
  }
  catch (std::exception &) {
    removeTempFile(file);
    throw;
  }

  removeTempFile(file);

  return clubId;
}

void TabCompetition::getAPIKey(vector< pair<string, string> > &key) const {
  string apikey = oe->getPropertyStringDecrypt("apikey", "");

  if (apikey.empty() || organizorId == 0)
    throw std::exception("Internal error");

  key.clear();
  key.push_back(pair<string, string>("ApiKey", apikey));
}

void TabCompetition::getEventorCompetitions(gdioutput &gdi,
                                            const string &fromDate,
                                            vector<CompetitionInfo> &events) const
{
  events.clear();

  vector< pair<string, string> > key;
  getAPIKey(key);

  string file = getTempFile();
  string url = eventorBase + "events?fromDate=" + fromDate +
              "&organisationIds=" + itos(organizorId) + "&includeEntryBreaks=true";
  Download dwl;
  dwl.initInternet();

  try {
    dwl.downloadFile(url, file, key);
  }
  catch (std::exception &) {
    removeTempFile(file);
    throw;
  }

  dwl.createDownloadThread();
  while (dwl.isWorking()) {
    Sleep(100);
  }
  xmlparser xml(0);
  xmlList xmlEvents;

  try {
    xml.read(file.c_str());
    xmlobject obj = xml.getObject("EventList");
    obj.getObjects("Event", xmlEvents);
  }
  catch (std::exception &) {
    removeTempFile(file);
    throw;
  }

  removeTempFile(file);

  for (size_t k = 0; k < xmlEvents.size(); k++) {
    CompetitionInfo ci;
    xmlEvents[k].getObjectString("Name", ci.Name);
    ci.Id = xmlEvents[k].getObjectInt("EventId");
    xmlobject date = xmlEvents[k].getObject("StartDate");
    date.getObjectString("Date", ci.Date);
    if (date.getObject("Clock"))
      date.getObjectString("Clock", ci.firstStart);

    if (useEventorUTC()) {
      int offset = getTimeZoneInfo(ci.Date);
      int t = convertAbsoluteTimeISO(ci.firstStart);
      int nt = t - offset;
      int dayOffset = 0;
      if (nt < 0) {
        nt += 24*3600;
        dayOffset = -1;
      }
      else if (nt > 24*3600) {
        nt -= 24*3600;
        dayOffset = 1;
      }
      ci.firstStart = formatTimeHMS(nt);
      //TODO: Take dayoffset into account
    }

    xmlEvents[k].getObjectString("WebURL", ci.url);
    xmlobject aco = xmlEvents[k].getObject("Account");
    if (aco) {
      string type = aco.getAttrib("type").get();
      string no;
      aco.getObjectString("AccountNo", no);

      if (type == "bankGiro")
        ci.account = "BG " + no;
      else if (type == "postalGiro")
        ci.account = "PG " + no;
      else
        ci.account = no;
    }

    ci.lastNormalEntryDate = "";
    xmlList entryBreaks;
    xmlEvents[k].getObjects("EntryBreak", entryBreaks);
    /* Mats Troeng explains Entry Break 2011-04-03:
    Efteranm�lan i detta fall �r satt som en till�ggsavgift (+50%) p� ordinarie avgift.
    Till�ggsavgiften �r aktiv 2011-04-13 -- 2011-04-20, medan den ordinarie avgiften �r aktiv -- 2011-04-20. Man kan ocks�
    definiera enligt ditt andra exempel i Eventor om man vill, men d� m�ste man s�tta ett fixt belopp i st�llet f�r en
    procentsats f�r efteranm�lan eftersom det inte finns n�got belopp att ber�kna procentsatsen p�.

    F�r att f� ut anm�lningsstoppen f�r en t�vling tittar man allts� p� unionen av alla (ValidFromDate - 1 sekund)
    samt ValidToDate. I normalfallet �r det tv� stycken, varav det f�rsta �r ordinarie anm�lningsstopp.
    F�r t ex O-Ringen som har flera anm�lningsstopp blir det mer �n tv� EntryBreaks.
    */
    for (size_t k = 0; k<entryBreaks.size(); k++) {
      xmlobject eBreak = entryBreaks[k].getObject("ValidFromDate");
      if (eBreak) {
        string breakDate;
        eBreak.getObjectString("Date", breakDate);

        SYSTEMTIME st;
        convertDateYMS(breakDate, st, false);
        __int64 time = SystemTimeToInt64Second(st) - 1;
        breakDate = convertSystemDate(Int64SecondToSystemTime(time));

        if (ci.lastNormalEntryDate.empty() || ci.lastNormalEntryDate >= breakDate)
          ci.lastNormalEntryDate = breakDate;
      }

      eBreak = entryBreaks[k].getObject("ValidToDate");
      if (eBreak) {
        string breakDate;
        eBreak.getObjectString("Date", breakDate);
        if (ci.lastNormalEntryDate.empty() || ci.lastNormalEntryDate >= breakDate)
          ci.lastNormalEntryDate = breakDate;

      }
    }

    events.push_back(ci);
  }
}

void TabCompetition::getEventorCmpData(gdioutput &gdi, int id,
                                       const string &eventFile,
                                       const string &clubFile,
                                       const string &classFile,
                                       const string &entryFile,
                                       const string &dbFile) const
{
  ProgressWindow pw(gdi.getHWND());
  pw.init();
  gdi.fillDown();
  gdi.addString("", 1, "Ansluter till Internet").setColor(colorGreen);
  gdi.dropLine(0.5);
  gdi.refreshFast();
  Download dwl;
  dwl.initInternet();

  pw.setProgress(1);
  vector< pair<string, string> > key;
  string apikey = oe->getPropertyStringDecrypt("apikey", "");
  key.push_back(pair<string, string>("ApiKey", apikey));

  gdi.fillRight();

  int prg = 0;
  int event_prg = dbFile.empty() ? 1000 / 4 : 1000/6;
  int club_prg = event_prg;

  if (id > 0) {
    gdi.addString("", 0, "H�mtar t�vling...");
    gdi.refreshFast();
    dwl.downloadFile(eventorBase + "export/event?eventId=" + itos(id) + iofExportVersion, eventFile, key);
    dwl.createDownloadThread();
    while (dwl.isWorking()) {
      Sleep(100);
    }
    if (!dwl.successful())
      throw std::exception("Download failed");

    prg += int(event_prg * 0.2);
    pw.setProgress(prg);
    gdi.addString("", 0, "OK");
    gdi.popX();
    gdi.dropLine();

    gdi.addString("", 0, "H�mtar klasser...");
    gdi.refreshFast();
    dwl.downloadFile(eventorBase + "export/classes?eventId=" + itos(id) + iofExportVersion, classFile, key);
    dwl.createDownloadThread();
    while (dwl.isWorking()) {
      Sleep(100);
    }

    if (!dwl.successful())
      throw std::exception("Download failed");

    prg += event_prg;
    pw.setProgress(prg);
    gdi.addString("", 0, "OK");
    gdi.popX();
    gdi.dropLine();


    gdi.addString("", 0, "H�mtar anm�lda...");
    gdi.refreshFast();
    dwl.downloadFile(eventorBase + "export/entries?eventId=" + itos(id) + iofExportVersion, entryFile, key);
    dwl.createDownloadThread();
    while (dwl.isWorking()) {
      Sleep(100);
    }
    if (!dwl.successful())
      throw std::exception("Download failed");

    prg += int(event_prg * 1.8);
    pw.setProgress(prg);
    gdi.addString("", 0, "OK");
    gdi.popX();
    gdi.dropLine();
  }


  gdi.addString("", 0, "H�mtar klubbar...");
  gdi.refreshFast();
  dwl.downloadFile(eventorBase + "export/clubs?" + iofExportVersion, clubFile, key);
  dwl.createDownloadThread();
  while (dwl.isWorking()) {
    Sleep(100);
  }
  if (!dwl.successful())
    throw std::exception("Download failed");

  prg += club_prg;
  pw.setProgress(prg);
  gdi.addString("", 0, "OK");
  gdi.popX();
  gdi.dropLine();

  if (dbFile.length() > 0) {
    gdi.addString("", 0, "H�mtar l�pardatabasen...");
    gdi.refreshFast();
    dwl.downloadFile(eventorBase + "export/cachedcompetitors?organisationIds=1&includePreselectedClasses=false&zip=true" + iofExportVersion, dbFile, key);
    dwl.createDownloadThread();
    while (dwl.isWorking()) {
      Sleep(100);
    }

    if (!dwl.successful())
      throw std::exception("Download failed");

    pw.setProgress(1000);
    gdi.addString("", 0, "OK");
  }

  gdi.popX();
  gdi.dropLine();
}

void TabCompetition::saveMultiEvent(gdioutput &gdi) {
  ListBoxInfo lbiPre, lbiPost;

  gdi.getSelectedItem("PreEvent", lbiPre);
  gdi.getSelectedItem("PostEvent", lbiPost);

  int idPost = lbiPost.data;
  int idPre = lbiPre.data;

  string nameIdPost = oe->getNameId(idPost);
  string nameIdPre = oe->getNameId(idPre);
  string nameId = oe->getNameId(0);
  if (nameIdPost == nameId || nameIdPre == nameId || (nameIdPost == nameIdPre && !nameIdPost.empty()))
    throw meosException("Ogiltig f�reg�ende/efterf�ljande etapp.");

  if (idPost == -2)
    oe->getDI().setString("PostEvent", "");
  else if (!nameIdPost.empty())
    oe->getDI().setString("PostEvent", nameIdPost);

  if (idPre == -2)
    oe->getDI().setString("PreEvent", "");
  else if (!nameIdPre.empty())
    oe->getDI().setString("PreEvent", nameIdPre);
}

void TabCompetition::loadMultiEvent(gdioutput &gdi) {
  if (oe->isClient()) {
    throw meosException("info:multieventnetwork");
  }

  gdi.clearPage(false);
  gdi.addString("", boldLarge, "Hantera flera etapper");

  gdi.setRestorePoint("MultiHeader");
  gdi.dropLine();

  gdi.pushX();
  gdi.fillRight();

  string preEvent = oe->getDCI().getString("PreEvent");
  string postEvent = oe->getDCI().getString("PostEvent");

  gdi.addSelection("PreEvent", 300, 200, CompetitionCB, "F�reg�ende etapp:", "V�lj den etapp som f�reg�r denna t�vling");
  char bf[260];
  getUserFile(bf, "");
  oe->enumerateCompetitions(bf, "*.meos");

  oe->fillCompetitions(gdi, "PreEvent", 1, preEvent);
  gdi.addItem("PreEvent", lang.tl("Ingen / ok�nd"), -2);
  bool hasPre = !gdi.getText("PreEvent").empty();
  if (!hasPre)
    gdi.selectItemByData("PreEvent", -2);

  gdi.addSelection("PostEvent", 300, 200, CompetitionCB, "N�sta etapp:", "V�lj den etapp som kommer efter denna t�vling");
  oe->fillCompetitions(gdi, "PostEvent", 1,  postEvent);
  gdi.addItem("PostEvent", lang.tl("Ingen / ok�nd"), -2);
  bool hasPost = !gdi.getText("PostEvent").empty();

  if (!hasPost)
    gdi.selectItemByData("PostEvent", -2);

  gdi.dropLine(5);
  gdi.popX();
  gdi.fillRight();

  int numStages = oe->getNumStages();
  gdi.addSelection("StageNumber", 100, 200, CompetitionCB, "Denna etapps nummer:");
  gdi.addItem("StageNumber", lang.tl("Inget nummer"), -2);
  for (int k = 1; k <= 52; k++)
    gdi.addItem("StageNumber", lang.tl("Etapp X#" + itos(k)), k);
  int sn = oe->getStageNumber();
  if (sn>=1 && sn <= 52) {
    gdi.selectItemByData("StageNumber", sn);
    if (oe->hasNextStage())
      numStages = max(numStages, sn+1);
    else
      numStages = max(numStages, sn);

    oe->setNumStages(numStages);
    oe->synchronize(true);
  }
  else
    gdi.selectFirstItem("StageNumber");

  gdi.fillDown();
  gdi.addInput("NumStages", numStages > 0 ? itos(numStages) : _EmptyString, 4, CompetitionCB, "Totalt antal etapper:");

  gdi.fillRight();
  gdi.dropLine(2);
  gdi.addButton("OpenPre", "�ppna f�reg�ende", CompetitionCB, "�ppna n�sta etapp");
  gdi.addButton("OpenPost", "�ppna n�sta", CompetitionCB, "�ppna f�reg�ende etapp");

  gdi.dropLine(3);
  gdi.popX();

  gdi.addButton("SaveMulti", "Spara", CompetitionCB);
  gdi.addButton("CloneCmp", "L�gg till ny etapp...", CompetitionCB);
  gdi.addButton("TransferData", "�verf�r resultat till n�sta etapp", CompetitionCB);
  gdi.addButton("Cancel", "�terg�", CompetitionCB);

  gdi.setInputStatus("OpenPre", hasPre);
  gdi.setInputStatus("OpenPost", hasPost);
  gdi.setInputStatus("TransferData", hasPost);
  gdi.setInputStatus("CloneCmp", !hasPost);

  gdi.refresh();
}

void TabCompetition::loadRunnerDB(gdioutput &gdi, int tableToShow, bool updateTableOnly) {
  if (!updateTableOnly) {
    gdi.clearPage(false);
    gdi.addString("", boldLarge, "L�pardatabasen");

    gdi.setRestorePoint("DBHeader");
  }
  else {
    gdi.restore("DBHeader", false);
  }
  gdi.dropLine();
  gdi.pushX();
  gdi.fillRight();
  gdi.addButton("RunnerDB", "Personer", CompetitionCB, "Visa l�pardatabasen");
  gdi.addButton("ClubDB", "Klubbar", CompetitionCB, "Visa klubbdatabasen");
  gdi.addButton("DBaseIn", "Importera", CompetitionCB, "Importera IOF (xml)");
  if (useEventor())
    gdi.addButton("EventorUpdateDB", "Uppdatera", CompetitionCB, "Uppdatera fr�n Eventor");
  gdi.addButton("ExportSetup", "Exportera", CompetitionCB, "Exportera p� fil");
  gdi.addButton("Cancel", "�terg�", CompetitionCB);

  gdi.dropLine(3);
  gdi.popX();

  //if (tableToShow != 0) {
    gdi.fillRight();
    gdi.addButton("ExportRunnerDB", "Exportera personer (IOF-XML)", CompetitionCB);
    gdi.addButton("ExportClubDB", "Exportera klubbar (IOF-XML)", CompetitionCB);
    gdi.addButton("ClearDB", "T�m databasen", CompetitionCB);
    gdi.dropLine(3);
    gdi.popX();

    if (oe->isClient()) {
      gdi.fillDown();
      gdi.addString("", 10, "info:runnerdbonline");
      gdi.dropLine();
      //gdi.disableInput("ExportRunnerDB");
      //gdi.disableInput("ExportClubDB");
      gdi.disableInput("ClearDB");
    }
  //}

  if (tableToShow == 1) {
    oe->updateRunnerDatabase();
    Table *tb = oe->getRunnerDatabase().getRunnerTB();
    gdi.addTable(tb, 40, gdi.getCY());
    gdi.registerEvent("CellAction", CompetitionCB);
  }
  else if (tableToShow == 2) {
    oe->updateRunnerDatabase();
    Table *tb = oe->getRunnerDatabase().getClubTB();
    gdi.addTable(tb, 40,  gdi.getCY());
  }

  gdi.refresh();
}

void TabCompetition::welcomeToMeOS(gdioutput &gdi) {
  gdi.clearPage(false, false);
  gdi.scaleSize(1.8/gdi.getScale());
  gdi.dropLine(5);
  gdi.setCX(gdi.getCX() + 5*gdi.getLineHeight());

  gdi.addString("", 2, "V�lkommen till MeOS");
  gdi.addString("", 1, MakeDash("#- ")+ lang.tl("ett Mycket Enkelt OrienteringsSystem")).setColor(colorDarkBlue);
  gdi.dropLine();
  gdi.addString("", 0, getMeosFullVersion());
  gdi.dropLine(2);
  gdi.addStringUT(0, "V�lj spr�k / Preferred language / Sprache");
  gdi.dropLine();
  gdi.fillRight();
  const char *id = "Language";
  gdi.addSelection(id, 90, 200, CompetitionCB);
  vector<string> ln = lang.get().getLangResource();
  string current = oe->getPropertyString("Language", "Svenska");
  int ix = -1;
  for (size_t k = 0; k<ln.size(); k++) {
    gdi.addItem(id, ln[k], k);
    if (ln[k] == current)
      ix = k;
  }
  gdi.selectItemByData(id, ix);

  gdi.addButton("WelcomeOK", "OK", CompetitionCB);
  gdi.dropLine(8);
  gdi.updatePos(gdi.getWidth(), gdi.getCX(), 5*gdi.getLineHeight(), 0);
  gdi.refresh();
}

void TabCompetition::displayRunners(gdioutput &gdi, const vector<pRunner> &changedClass) const {
  for (size_t k = 0; k<changedClass.size(); k++) {
    gdi.addStringUT(0, changedClass[k]->getName() + " (" + changedClass[k]->getClass() +", " +
                       changedClass[k]->getStartTimeS()+ ")");
  }
}

void TabCompetition::selectTransferClasses(gdioutput &gdi, bool expand) {
  gdi.restore("SelectTClass", false);
  gdi.setRestorePoint("SelectTClass");

  gdi.fillDown();
  gdi.addSelection("ChangeClassType", 300, 400, 0, "Hantera deltagare som bytt klass:");
  gdi.addItem("ChangeClassType", lang.tl("Byt till vakansplats i r�tt klass (om m�jligt)"), oEvent::ChangeClassVacant);
  gdi.addItem("ChangeClassType", lang.tl("Byt till r�tt klass (beh�ll eventuell starttid)"), oEvent::ChangeClass);
  gdi.addItem("ChangeClassType", lang.tl("Till�t ny klass, inget totalresultat"), oEvent::TransferNoResult);
  gdi.addItem("ChangeClassType", lang.tl("Till�t ny klass, beh�ll resultat fr�n annan klass"), oEvent::TransferAnyway);
  gdi.selectItemByData("ChangeClassType", lastChangeClassType);
  
  if (expand) {
    gdi.fillDown();
    gdi.addListBox("ClassNewEntries", 200, 400, 0, "Klasser d�r nyanm�lningar ska �verf�ras:", "", true);
    oe->fillClasses(gdi, "ClassNewEntries", oEvent::extraNone, oEvent::filterNone);

    gdi.setSelection("ClassNewEntries", allTransfer);
    gdi.pushX();
    gdi.fillRight();
    gdi.addButton("SelectAll", "V�lj allt", CompetitionCB);
    gdi.fillDown();
    gdi.addButton("SelectNone", "V�lj inget", CompetitionCB);
    gdi.popX();
    gdi.addCheckbox("TransferEconomy", "�verf�r nya deltagare i ej valda klasser med status \"deltar ej\"");
    gdi.fillRight();
  }
  else {
    gdi.fillRight();
    gdi.addButton("ExpandTResults", "V�lj klasser med nya anm�lningar", CompetitionCB);
  }

  gdi.addButton("DoTransferData", "�verf�r resultat", CompetitionCB);
  gdi.addButton("MultiEvent", "�terg�", CompetitionCB);
  gdi.popX();
  gdi.dropLine();
  gdi.refresh();
}

static int ClearFeaturesCB(gdioutput *gdi, int type, void *data)
{
  TabCompetition &tc = dynamic_cast<TabCompetition &>(*gdi->getTabs().get(TCmpTab));
  tc.saveMeosFeatures(*gdi, true);
  return 1;
}

static int CheckFeaturesCB(gdioutput *gdi, int type, void *data)
{
  TabCompetition &tc = dynamic_cast<TabCompetition &>(*gdi->getTabs().get(TCmpTab));
  tc.saveMeosFeatures(*gdi, false);
  tc.updateFeatureStatus(*gdi);
  return 0;
}

void TabCompetition::meosFeatures(gdioutput &gdi, bool newGuide) {
  if (!newGuide) {
    oe->checkNecessaryFeatures();
    gdi.clearPage(false);
    gdi.addString("", boldLarge, MakeDash("MeOS - Funktioner"));
  }
  else {
    gdi.dropLine();
    gdi.addString("", fontMediumPlus, MakeDash("MeOS - Funktioner"));
  }
  gdi.dropLine(0.5);

  const MeOSFeatures &mf = oe->getMeOSFeatures();
  int yp = gdi.getCY();
  int tx, ty;
  gdi.getTargetDimension(tx, ty);
  ty = max(ty-gdi.scaleLength(150), 300);
  int nf = mf.getNumFeatures();
  int maxLen = gdi.scaleLength(150);
  for (int k = 0; k < nf; k++) {
    if (mf.isHead(k)) {
      if (gdi.getCY() > ty) {
        //gdi.newColumn();
        gdi.setCX(gdi.getCX() + maxLen + gdi.scaleLength(10));
        maxLen = gdi.scaleLength(150);
        gdi.setCY(yp);
      }
      gdi.dropLine(0.6);
      TextInfo &ti = gdi.addString("", fontMediumPlus, mf.getHead(k));
      maxLen = max<int>(maxLen, ti.textRect.right - ti.textRect.left);
      gdi.dropLine(0.4);
    }
    else {
      MeOSFeatures::Feature f = mf.getFeature(k);
      ButtonInfo &bi = gdi.addCheckbox("feat" + mf.getCode(f), mf.getDescription(f),
                                        CheckFeaturesCB, mf.hasFeature(f));
      maxLen = max<int>(maxLen, bi.width);

      if (mf.isRequired(f, *oe))
        gdi.setInputStatus("feat" + mf.getCode(f), false);
    }
  }

  gdi.dropLine();

  if (!newGuide) {
    gdi.fillRight();
    gdi.addButton("SaveFeaures", "Spara", CompetitionCB).setDefault();
    gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();
    gdi.setOnClearCb(ClearFeaturesCB);

    gdi.refresh();
  }

}

void TabCompetition::updateFeatureStatus(gdioutput &gdi) {
  const MeOSFeatures &mf = oe->getMeOSFeatures();
  int nf = mf.getNumFeatures();
  for (int k = 0; k < nf; k++) {
    if (!mf.isHead(k)) {
      MeOSFeatures::Feature f = mf.getFeature(k);
      string id = "feat" + mf.getCode(f);
      gdi.check(id, mf.hasFeature(f));
      gdi.setInputStatus(id, !mf.isRequired(f, *oe));
    }
  }
  gdi.refresh();
}


void TabCompetition::saveMeosFeatures(gdioutput &gdi, bool write) {
  MeOSFeatures &mf = oe->getMeOSFeatures();

  int nf = mf.getNumFeatures();
  for (int k = 0; k < nf; k++) {
    if (!mf.isHead(k)) {
      MeOSFeatures::Feature f = mf.getFeature(k);
      string key = "feat" + mf.getCode(f);
      mf.useFeature(f, gdi.isChecked(key), *oe);
    }
  }
  if (write) {
    oe->getDI().setString("Features", mf.serialize());
    oe->synchronize(true);
  }
}

void TabCompetition::entryForm(gdioutput &gdi, bool isGuide) {
  if (isGuide) {
    gdi.dropLine(1);
    gdi.addString("", fontMediumPlus, "Importera t�vlingsdata");
  }
  else
    gdi.addString("", 2, "Importera t�vlingsdata");

  gdi.dropLine(0.5);
  gdi.addString("", 10, "help:import_entry_data");
  gdi.dropLine();

  gdi.pushX();

  gdi.fillRight();
  gdi.addInput("FileNameCmp", "", 48, 0, "T�vlingsinst�llningar (IOF, xml)");
  gdi.dropLine();
  gdi.addButton("BrowseEntries", "Bl�ddra...", CompetitionCB).setExtra("FileNameCmp");
  gdi.popX();

  gdi.dropLine(2.5);
  gdi.addInput("FileNameCls", "", 48, 0, "Klasser (IOF, xml)");
  gdi.dropLine();
  gdi.addButton("BrowseEntries", "Bl�ddra...", CompetitionCB).setExtra("FileNameCls");
  gdi.popX();

  gdi.dropLine(2.5);
  gdi.addInput("FileNameClb", "", 48, 0, "Klubbar (IOF, xml)");
  gdi.dropLine();
  gdi.addButton("BrowseEntries", "Bl�ddra...", CompetitionCB).setExtra("FileNameClb");
  gdi.popX();

  gdi.dropLine(2.5);
  gdi.addInput("FileName", "", 48, 0, "Anm�lningar (IOF (xml) eller OE-CSV)");
  gdi.dropLine();
  gdi.addButton("BrowseEntries", "Bl�ddra...", CompetitionCB).setExtra("FileName");
  gdi.popX();
  gdi.dropLine(2.2);
	if (!isGuide && oe->getNumRunners() > 0) {
		gdi.addCheckbox("RemoveRemoved", "Ta bort eventuella avanm�lda deltagare", 0, true);
	}
  gdi.addCheckbox("ReverseNames", "Import names as \"surname, first name\"", 0, true);
  gdi.popX();
  gdi.dropLine(1.5);
  gdi.addCheckbox("UseFFCOClubMapping", "Use French Federation of Orienteering mapping", 0, oe->getPropertyString("Language", "English") == "Fran�ais");
  gdi.popX();
  

  gdi.dropLine(2.5);
  gdi.addInput("FileNameRank", "", 48, 0, "Ranking (IOF, xml)");
  gdi.dropLine();
  gdi.addButton("BrowseEntries", "Bl�ddra...", CompetitionCB).setExtra("FileNameRank");
  gdi.popX();
  gdi.fillDown();
  gdi.dropLine(3);
}

void TabCompetition::saveEntries(gdioutput &gdi, bool removeRemoved, bool isGuide, bool reverseNames, bool useFFCOClubmapping) {
  string filename[5];
  filename[0] = gdi.getText("FileNameCmp");
  filename[1] = gdi.getText("FileNameCls");
  filename[2] = gdi.getText("FileNameClb");
  filename[3] = gdi.getText("FileName");
  filename[4] = gdi.getText("FileNameRank");

  csvparser csv;

  for (int i = 0; i<5; i++) {
    if (filename[i].empty())
      continue;

    gdi.addString("", 0, "Behandlar: X#" + filename[i]);

    int type=csv.iscsv(filename[i].c_str());

    if (type) {
      const char *File = filename[i].c_str();

      if (type==1) {
        gdi.addString("", 0, "Importerar OE2003 csv-fil...");
        gdi.refresh();
        gdi.setWaitCursor(true);
        if (csv.ImportOE_CSV(*oe, File, reverseNames, useFFCOClubmapping)) {
          gdi.addString("", 0, "Klart. X deltagare importerade.#" + itos(csv.nimport));
        }
        else gdi.addString("", 0, "F�rs�ket misslyckades.");
      }
      else if (type==2) {
        gdi.addString("", 0, "Importerar OS2003 csv-fil...");
        gdi.refresh();
        gdi.setWaitCursor(true);
        if (csv.ImportOS_CSV(*oe, File)) {
          gdi.addString("", 0, "Klart. X lag importerade.#" + itos(csv.nimport));
        }
        else gdi.addString("", 0, "F�rs�ket misslyckades.");
      }
      else if (type==3) {
        gdi.addString("", 0, "Importerar RAID patrull csv-fil...");
        gdi.setWaitCursor(true);
        if (csv.ImportRAID(*oe, File)) {
          gdi.addString("", 0, "Klart. X patruller importerade.#" + itos(csv.nimport));
        }
        else gdi.addString("", 0, "F�rs�ket misslyckades.");

      }
    }
    else {
      oe->importXML_EntryData(gdi, filename[i].c_str(), false, removeRemoved);
    }
    if (!isGuide) {
      gdi.setWindowTitle(oe->getTitleName());
      oe->updateTabs();
    }
  }
}


void TabCompetition::selectStartlistOptions(gdioutput &gdi) {
  gdi.clearPage(true);
  gdi.addString("", boldLarge, "Exportera startlista");
  gdi.pushY();
  gdi.addListBox("ClassNewEntries", 250, 400, 0, "Klassval:", "", true);
  oe->fillClasses(gdi, "ClassNewEntries", oEvent::extraNone, oEvent::filterNone);

  gdi.setSelection("ClassNewEntries", allTransfer);
  gdi.pushX();
  gdi.fillRight();
  gdi.addButton("SelectAll", "V�lj allt", CompetitionCB);
  gdi.fillDown();
  gdi.addButton("SelectNone", "V�lj inget", CompetitionCB);
  gdi.popX();

  gdi.newColumn();
  gdi.pushX();
  gdi.popY();
  gdi.addSelection("Type", 250, 200, CompetitionCB, "Exporttyp:");

  vector< pair<string, size_t> > types;
  types.push_back(make_pair(lang.tl("IOF Startlista, version 3.0 (xml)"), 1));
  types.push_back(make_pair(lang.tl("IOF Startlista, version 2.0.3 (xml)"), 2));
  types.push_back(make_pair(lang.tl("OE Semikolonseparerad (csv)"), 3));
  types.push_back(make_pair(lang.tl("Webbdokument (html)"), 4));

  gdi.addItem("Type", types);
  gdi.selectFirstItem("Type");


  gdi.addSelection("LanguageType", 250, 200, CompetitionCB, "Export language");
  vector<pair<string, size_t>> typeLanguages;
  typeLanguages.push_back(make_pair(lang.tl("English"), 1));
  typeLanguages.push_back(make_pair(lang.tl("Svenska"), 2));
  typeLanguages.push_back(make_pair(lang.tl("Deutsch"), 3));
  typeLanguages.push_back(make_pair(lang.tl("Dansk"), 4));
  typeLanguages.push_back(make_pair(lang.tl("Fran�ais"), 5));
  typeLanguages.push_back(make_pair(lang.tl("Russian"), 6));

  gdi.addItem("LanguageType", typeLanguages);
  string currentLanguage = oe->getPropertyString("Language", "English");
  int defaultLanguageType = 1;

  if (currentLanguage == "English")
	  defaultLanguageType = 1;
  else if (currentLanguage == "Svenska")
	  defaultLanguageType = 2;
  else if (currentLanguage == "Deutsch")
	  defaultLanguageType = 3;
  else if (currentLanguage == "Dansk")
	  defaultLanguageType = 4;
  else if (currentLanguage == "Fran�ais")
	  defaultLanguageType = 5;
  else if (currentLanguage == "Russian(ISO 8859 - 5)")
	  defaultLanguageType = 6;

  gdi.selectItemByData("LanguageType", defaultLanguageType);
  gdi.disableInput("LanguageType");
  gdi.addCheckbox("UseFFCOClubMapping", "Use French Federation of Orienteering mapping", 0, defaultLanguageType == 5);
  gdi.disableInput("UseFFCOClubMapping");

  ClassConfigInfo cnf;
  oe->getClassConfigurationInfo(cnf);

  if (oe->hasTeam()) {
    gdi.addCheckbox("ExportTeam", "Exportera individuella lopp ist�llet f�r lag", 0, false);
  }
  if (oe->hasMultiRunner() || oe->getStageNumber() > 0)
    gdi.addCheckbox("IncludeRaceNumber", "Inkludera information om flera lopp per l�pare", 0, true);

  gdi.addInput("Filename", "", 48, CompetitionCB,  "Filnamn:").setExtra("DoSaveStartlist");
  gdi.fillRight();
  gdi.dropLine();
  gdi.addButton("BrowseExport", "Bl�ddra...",  CompetitionCB);
  gdi.addButton("DoSaveStartlist", "Exportera",  CompetitionCB).setDefault();
  gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();
  gdi.disableInput("DoSaveStartlist");
  gdi.refresh();
}

void TabCompetition::selectExportSplitOptions(gdioutput &gdi) {
  gdi.clearPage(false);
  gdi.addString("", boldLarge, "Export av resultat/str�cktider");
  gdi.dropLine();
  gdi.pushY();
  gdi.addListBox("ClassNewEntries", 250, 400, 0, "Klassval:", "", true);
  oe->fillClasses(gdi, "ClassNewEntries", oEvent::extraNone, oEvent::filterNone);

  gdi.setSelection("ClassNewEntries", allTransfer);
  gdi.pushX();
  gdi.fillRight();
  gdi.addButton("SelectAll", "V�lj allt", CompetitionCB);
  gdi.fillDown();
  gdi.addButton("SelectNone", "V�lj inget", CompetitionCB);
  gdi.popX();
  gdi.newColumn();
  gdi.popY();
  gdi.pushX();
  gdi.addSelection("Type", 250, 200, CompetitionCB, "Exporttyp");

  vector< pair<string, size_t> > types;
  types.push_back(make_pair(lang.tl("IOF Resultat, version 3.0 (xml)"), 1));
  types.push_back(make_pair(lang.tl("IOF Resultat, version 2.0.3 (xml)"), 2));
  types.push_back(make_pair(lang.tl("OE Semikolonseparerad (csv)"), 3));
  types.push_back(make_pair(lang.tl("Webbdokument (html)"), 4));

  gdi.addItem("Type", types);
  gdi.selectFirstItem("Type");

  gdi.addSelection("LanguageType", 250, 200, CompetitionCB, "Export language");
  vector<pair<string, size_t>> typeLanguages;
  typeLanguages.push_back(make_pair(lang.tl("English"), 1));
  typeLanguages.push_back(make_pair(lang.tl("Svenska"), 2));
  typeLanguages.push_back(make_pair(lang.tl("Deutsch"), 3));
  typeLanguages.push_back(make_pair(lang.tl("Dansk"), 4));
  typeLanguages.push_back(make_pair(lang.tl("Fran�ais"), 5));
  typeLanguages.push_back(make_pair(lang.tl("Russian"), 6));

  gdi.addItem("LanguageType", typeLanguages);
  string currentLanguage = oe->getPropertyString("Language", "English");
  int defaultLanguageType = 1;

  if (currentLanguage == "English")
	  defaultLanguageType = 1;
  else if (currentLanguage == "Svenska")
	  defaultLanguageType = 2;
  else if (currentLanguage == "Deutsch")
	  defaultLanguageType = 3;
  else if (currentLanguage == "Dansk")
	  defaultLanguageType = 4;
  else if (currentLanguage == "Fran�ais")
	  defaultLanguageType = 5;
  else if (currentLanguage == "Russian(ISO 8859 - 5)")
	  defaultLanguageType = 6;

  gdi.selectItemByData("LanguageType", defaultLanguageType);
  gdi.disableInput("LanguageType");
  gdi.addCheckbox("ExportSplitTimes", "Export Split Times", 0, false);
  gdi.disableInput("ExportSplitTimes");
  gdi.addCheckbox("UseFFCOClubMapping", "Use French Federation of Orienteering mapping", 0, defaultLanguageType == 5);
  gdi.disableInput("UseFFCOClubMapping");

  ClassConfigInfo cnf;
  oe->getClassConfigurationInfo(cnf);

  if (oe->hasTeam()) {
    gdi.addSelection("LegType", 300, 100, 0, "Exportval, IOF-XML");
    gdi.addItem("LegType", lang.tl("Totalresultat"), 1);
    gdi.addItem("LegType", lang.tl("Alla lopp som individuella"), 3);
    gdi.addItem("LegType", lang.tl("Alla str�ckor/lopp i separata filer"), 2);
    int legMax = cnf.getNumLegsTotal();
    for (int k = 0; k<legMax; k++) {
      gdi.addItem("LegType", lang.tl("Str�cka X#" + itos(k+1)), k+10);
    }
    gdi.selectFirstItem("LegType");
  }

  bool hasLoops = false;
  vector<pCourse> crs;
  oe->getCourses(crs);
  for (size_t k = 0; k < crs.size(); k++) {
    if (crs[k]->getCommonControl() != 0)
      hasLoops = true;
  }
  if (hasLoops)
    gdi.addCheckbox("UnrollLoops", "Unroll split times for loop courses", 0, true);

  if (oe->hasMultiRunner() || oe->getStageNumber() > 0)
    gdi.addCheckbox("IncludeRaceNumber", "Inkludera information om flera lopp per l�pare", 0, true);

  gdi.addInput("Filename", "", 48, CompetitionCB,  "Filnamn:").setExtra("DoSaveSplits");
  gdi.fillRight();
  gdi.dropLine();
  gdi.addButton("BrowseExportResult", "Bl�ddra...",  CompetitionCB);
  gdi.addButton("DoSaveSplits", "Exportera",  CompetitionCB).setDefault();
  gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();

  gdi.disableInput("DoSaveSplits");
  gdi.refresh();
}

void TabCompetition::clearCompetitionData() {
}

void TabCompetition::loadSettings(gdioutput &gdi) {
  gdi.clearPage(false);

  gdi.addString("", boldLarge, "T�vlingsinst�llningar");
  gdi.dropLine(0.5);
  vector<string> fields;
  gdi.pushY();
  gdi.addString("", 1, "Adress och kontakt");
  fields.push_back("Organizer");
  fields.push_back("CareOf");
  fields.push_back("Street");
  fields.push_back("Address");
  fields.push_back("EMail");
  fields.push_back("Homepage");

  oe->getDI().buildDataFields(gdi, fields);

  gdi.dropLine();
  gdi.addString("", 1, "Tidszon");

  gdi.dropLine(0.3);
  gdi.addCheckbox("UTC", "Exportera tider i UTC", 0,
                  oe->getDCI().getInt("UTC") == 1);

  gdi.newColumn();
  gdi.popY();

  gdi.addString("", 1, "Avgifter");
  fields.clear();
  gdi.fillRight();
  gdi.pushX();
  fields.push_back("CardFee");
  fields.push_back("EliteFee");
  fields.push_back("EntryFee");
  fields.push_back("YouthFee");

  oe->getDI().buildDataFields(gdi, fields);

  gdi.popX();
  gdi.dropLine(3);

  fields.clear();
  fields.push_back("OrdinaryEntry");
  fields.push_back("LateEntryFactor");

  oe->getDI().buildDataFields(gdi, fields);

  gdi.fillDown();
  gdi.popX();
  gdi.dropLine(3);

  gdi.addString("", 1, "�ldersgr�nser, reducerad anm�lningsavgift");
  fields.clear();
  fields.push_back("YouthAge");
  fields.push_back("SeniorAge");
  gdi.fillRight();
  oe->getDI().buildDataFields(gdi, fields);

  gdi.fillDown();
  gdi.popX();
  gdi.dropLine(3);


  gdi.addString("", 1, "Valuta");
  fields.clear();
  fields.push_back("CurrencySymbol");
  fields.push_back("CurrencyCode");

  gdi.fillRight();
  oe->getDI().buildDataFields(gdi, fields);

  gdi.dropLine();
  gdi.addCheckbox("PreSymbol", "Valutasymbol f�re", 0,
                  oe->getDCI().getInt("CurrencyPreSymbol") == 1);

  gdi.popX();
  gdi.dropLine(3);
  bool useFrac = oe->getDCI().getInt("CurrencyFactor") == 100;
  gdi.addCheckbox("UseFraction", "Till�t decimaler", CompetitionCB,
                    useFrac, "Till�t valutauttryck med decimaler");

  fields.clear();
  gdi.dropLine(-1);
  fields.push_back("CurrencySeparator");
  oe->getDI().buildDataFields(gdi, fields);

  gdi.setInputStatus("CurrencySeparator_odc", useFrac);

  gdi.fillDown();
  gdi.popX();
  gdi.dropLine(3);

  gdi.addString("", 1, "Betalningsinformation");
  fields.clear();
  fields.push_back("Account");
  fields.push_back("PaymentDue");

  oe->getDI().buildDataFields(gdi, fields);

  gdi.fillDown();
  gdi.addString("", 1, "T�vlingsregler");
  fields.clear();
  gdi.fillRight();
  gdi.pushX();
  fields.push_back("MaxTime");
  oe->getDI().buildDataFields(gdi, fields);
  oe->getDI().fillDataFields(gdi);

  gdi.dropLine(3);
  int bottom = gdi.getCY();


  gdi.newColumn();
  gdi.popY();
  gdi.pushX();
  gdi.fillDown();
  gdi.addString("", 1, "Betalningsmetoder");
  gdi.dropLine();
  gdi.addString("", 10, "help:paymentmodes");
  gdi.dropLine();
  vector< pair<string, size_t> > modes;
  oe->getPayModes(modes);
  for (size_t k = 0; k < modes.size(); k++) {
    gdi.fillRight();
    string ms = itos(modes[k].second);
    gdi.addInput("M" + itos(k), modes[k].first, 24).setExtra(modes[k].second);
    if (k > 0)
      gdi.addButton(gdi.getCX(), gdi.getCY(), gdi.scaleLength(20), 
                    "RemovePayMode", MakeDash("-"), CompetitionCB, 
                    "Ta bort", false, false).setExtra(modes[k].second);
    if (k == 0) 
      gdi.addButton(gdi.getCX(), gdi.getCY(), gdi.scaleLength(20), 
                    "AddPayMode", "+", CompetitionCB, 
                    "L�gg till", false, false);

    gdi.dropLine(2.5);
    gdi.popX();
  }
  bottom = max(bottom, gdi.getCY());

  gdi.popX();
  gdi.setCY(bottom);
  gdi.fillRight();
  gdi.addButton("SaveSettings", "Spara", CompetitionCB).setDefault();
  gdi.addButton("Cancel", "Avbryt", CompetitionCB).setCancel();
  gdi.dropLine(2);
  gdi.setOnClearCb(CompetitionCB);
  gdi.refresh();

}

void TabCompetition::saveSettings(gdioutput &gdi) {
  vector<string> fields;
  vector<int> fees(4);
  fields.push_back("CardFee");
  fields.push_back("EliteFee");
  fields.push_back("EntryFee");
  fields.push_back("YouthFee");

  for (int k = 0; k<4; k++)
    fees[k] = oe->getDCI().getInt(fields[k]);
  string factor = oe->getDCI().getString("LateEntryFactor");
  oe->getDI().saveDataFields(gdi);

  bool changedFee = false;
  bool changedCardFee = false;

  for (int k = 0; k<4; k++) {
    if (fees[k] != oe->getDCI().getInt(fields[k])) {
      if (k > 0)
        changedFee = true;
      else {
        changedCardFee = true;
        if (oe->getDCI().getInt(fields[k]) == 0)
          oe->getDI().setInt(fields[k].c_str(), -1); // Disallow zero card fee. -1 means no fee.
      }
    }
  }
  if (factor != oe->getDCI().getString("LateEntryFactor"))
    changedFee = true;

  oe->getDI().setInt("UTC", gdi.isChecked("UTC") ? 1 : 0);

  oe->getDI().setInt("CurrencyFactor", gdi.isChecked("UseFraction") ? 100 : 1);
  oe->getDI().setInt("CurrencyPreSymbol", gdi.isChecked("PreSymbol") ? 1 : 0);
  oe->setCurrency(-1, "", "", false);

  vector< pair<string, size_t> > modes;
  oe->getPayModes(modes);
  for (size_t k = 0; k < modes.size(); k++) {
    string field = "M"+itos(k);
    if (gdi.hasField(field)) {
      string mode = gdi.getText("M"+itos(k));
      int id = gdi.getBaseInfo(field.c_str()).getExtraInt();
      oe->setPayMode(id, mode);
    }
  }

  // Read from model
  if (oe->isChanged()) {
    oe->setProperty("Organizer", oe->getDCI().getString("Organizer"));
    oe->setProperty("Street", oe->getDCI().getString("Street"));
    oe->setProperty("Address", oe->getDCI().getString("Address"));
    oe->setProperty("EMail", oe->getDCI().getString("EMail"));
    oe->setProperty("Homepage", oe->getDCI().getString("Homepage"));

    oe->setProperty("CardFee", oe->getDCI().getInt("CardFee"));
    oe->setProperty("EliteFee", oe->getDCI().getInt("EliteFee"));
    oe->setProperty("EntryFee", oe->getDCI().getInt("EntryFee"));
    oe->setProperty("YouthFee", oe->getDCI().getInt("YouthFee"));

    oe->setProperty("YouthAge", oe->getDCI().getInt("YouthAge"));
    oe->setProperty("SeniorAge", oe->getDCI().getInt("SeniorAge"));

    oe->setProperty("Account", oe->getDCI().getString("Account"));
    oe->setProperty("LateEntryFactor", oe->getDCI().getString("LateEntryFactor"));

    oe->setProperty("CurrencySymbol", oe->getDCI().getString("CurrencySymbol"));
    oe->setProperty("CurrencyFactor", oe->getDCI().getInt("CurrencyFactor"));
    oe->setProperty("CurrencyPreSymbol", oe->getDCI().getInt("CurrencyPreSymbol"));
    oe->setProperty("CurrencySeparator", oe->getDCI().getString("CurrencySeparator"));

    oe->setProperty("PayModes", oe->getDCI().getString("PayModes"));
  }
  oe->synchronize(true);
  set<int> dummy;
  if (changedFee && oe->getNumClasses() > 0) {
    bool updateFee = gdi.ask("ask:changedcmpfee");

    if (updateFee)
      oe->applyEventFees(true, true, changedCardFee, dummy);
  }
  else if (changedCardFee)
    oe->applyEventFees(false, false, true, dummy);
}
