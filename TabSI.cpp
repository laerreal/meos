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
#include <algorithm>

#include "oEvent.h"
#include "xmlparser.h"
#include "gdioutput.h"
#include "gdifonts.h"
#include "gdiconstants.h"

#include "csvparser.h"

#include "TabSI.h"
#include "TabAuto.h"
#include "TabList.h"
#include "meos_util.h"
#include <cassert>
#include "TabRunner.h"
#include "meosexception.h"
#include "MeOSFeatures.h"
#include "RunnerDB.h"
#include "recorder.h"

TabSI::TabSI(oEvent *poe):TabBase(poe) {
  editCardData.tabSI = this;
  interactiveReadout=poe->getPropertyInt("Interactive", 1)!=0;
  useDatabase=poe->useRunnerDb() && poe->getPropertyInt("Database", 1)!=0;
  printSplits = false;
  printStartInfo = false;
  savedCardUniqueId = 1;  
  
  manualInput = poe->getPropertyInt("ManualInput", 0) == 1;

  mode=ModeReadOut;
  currentAssignIndex=0;

  lastClubId=0;
  lastClassId=0;
  logger = 0;

  minRunnerId = 0;
  inputId = 0;

  NC = 8;
}

TabSI::~TabSI(void)
{
  if (logger!=0)
    delete logger;
  logger = 0;
}


static void entryTips(gdioutput &gdi) {
  gdi.fillDown();
  gdi.addString("", 10, "help:21576");
  gdi.dropLine(1);
  gdi.setRestorePoint("EntryLine");
}


void TabSI::logCard(const SICard &card)
{
  if (logger == 0) {
    logger = new csvparser;
    string readlog = "sireadlog_" + getLocalTimeFileName() + ".csv";
    char file[260];
    string subfolder = makeValidFileName(oe->getName(), true);
    const char *sf = subfolder.empty() ? 0 : subfolder.c_str();
    getDesktopFile(file, readlog.c_str(), sf);
    logger->openOutput(file);
    vector<string> head = SICard::logHeader();
    logger->OutputRow(head);
    logcounter = 0;
  }

  vector<string> log = card.codeLogData(++logcounter);
  logger->OutputRow(log);
}

extern SportIdent *gSI;
extern pEvent gEvent;

void LoadRunnerPage(gdioutput &gdi);


int SportIdentCB(gdioutput *gdi, int type, void *data)
{
  TabSI &tsi = dynamic_cast<TabSI &>(*gdi->getTabs().get(TSITab));

  return tsi.siCB(*gdi, type, data);
}

int TabSI::siCB(gdioutput &gdi, int type, void *data)
{
  if (type==GUI_BUTTON) {
    ButtonInfo bi=*(ButtonInfo *)data;

    if (bi.id == "ClearMemory") {
      if (gdi.ask("Do you want to clear the card memory?")) {
        savedCards.clear();
        loadPage(gdi);
      }
    }
    else if (bi.id == "SaveMemory") {
      vector< pair<string, string> > ext;
      ext.push_back(make_pair("Semikolonseparerad (csv)", "*.csv"));

      int filterIx = 0;
      string file = gdi.browseForSave(ext, "csv", filterIx);
      if (!file.empty()) {
        csvparser saver;
        saver.openOutput(file.c_str());
        vector<string> head = SICard::logHeader();
        saver.OutputRow(head);
        int count = 0;
        for (list< pair<int, SICard> >::const_iterator it = savedCards.begin(); it != savedCards.end(); ++it) {
          vector<string> log = it->second.codeLogData(++count);
          saver.OutputRow(log);
        }
      }
    }
    else if (bi.id == "CreateCompetition") {
      createCompetitionFromCards(gdi);
    }
    else if (bi.id=="SIPassive") {
      string port=gdi.getText("ComPortName");
      if (gSI->OpenComListen(port.c_str(), gdi.getTextNo("BaudRate"))) {
        gSI->StartMonitorThread(port.c_str());
        loadPage(gdi);
        gdi.addString("", 1, "Lyssnar p� X.#"+port).setColor(colorDarkGreen);
      }
      else
        gdi.addString("", 1, "FEL: Porten kunde inte �ppnas").setColor(colorRed);
      gdi.dropLine();
      gdi.refresh();
    }
    else if (bi.id=="CancelTCP")
      gdi.restore("TCP");
    else if (bi.id=="StartTCP") {
      gSI->tcpAddPort(gdi.getTextNo("tcpPortNo"), 0);
      gdi.restore("TCP");
      gSI->StartMonitorThread("TCP");

      gdi.addStringUT(0, gSI->getInfoString("TCP"));
      gdi.dropLine(0.5);
      refillComPorts(gdi);
      gdi.refresh();
    }
    else if (bi.id=="StartSI") {
      char bf[64];
      ListBoxInfo lbi;
      if (gdi.getSelectedItem("ComPort", lbi)) {

        sprintf_s(bf, 64, "COM%d", lbi.data);
        string port=bf;

        if (lbi.text.substr(0, 3)=="TCP")
          port="TCP";

        if (gSI->IsPortOpen(port)) {
          gSI->CloseCom(port.c_str());
          gdi.addStringUT(0, lang.tl("Kopplar ifr�n SportIdent p� ") + port + lang.tl("... OK"));
          gdi.popX();
          gdi.dropLine();
          refillComPorts(gdi);
        }
        else {
          gdi.fillDown();
          if (port=="TCP") {
            gdi.setRestorePoint("TCP");
            gdi.dropLine();
            gdi.pushX();
            gdi.fillRight();
            gdi.addInput("tcpPortNo", "10000", 8,0, "Port f�r TCP:");
            gdi.dropLine();
            gdi.addButton("StartTCP", "Starta", SportIdentCB);
            gdi.addButton("CancelTCP", "Avbryt", SportIdentCB);
            gdi.dropLine(2);
            gdi.popX();
            gdi.fillDown();
            gdi.addString("", 10, "help:14070");
            gdi.scrollToBottom();
            gdi.refresh();
            return 0;
          }

          gdi.addStringUT(0, lang.tl("Startar SI p�")+" "+ port + "...");
          gdi.refresh();
          if (gSI->OpenCom(port.c_str())){
            gSI->StartMonitorThread(port.c_str());
            gdi.addStringUT(0, lang.tl("SI p�")+" "+ port + ": "+lang.tl("OK"));
            gdi.addStringUT(0, gSI->getInfoString(port));
            SI_StationInfo *si = gSI->findStation(port);
            if (si && !si->Extended)
              gdi.addString("", boldText, "warn:notextended").setColor(colorDarkRed);
          }
          else{
            //Retry...
            Sleep(300);
            if (gSI->OpenCom(port.c_str())) {
              gSI->StartMonitorThread(port.c_str());
              gdi.addStringUT(0, lang.tl("SI p�") + " " + port + ": " + lang.tl("OK"));
              gdi.addStringUT(0, gSI->getInfoString(port.c_str()));
              SI_StationInfo *si = gSI->findStation(port);
              if (si && !si->Extended)
                gdi.addString("", boldText, "warn:notextended").setColor(colorDarkRed);
            }
            else {
              gdi.setRestorePoint();
              gdi.addStringUT(1, lang.tl("SI p�") +" "+ port + ": " + lang.tl("FEL, inget svar.")).setColor(colorRed);
              gdi.dropLine();
              gdi.refresh();

              if (gdi.ask("help:9615")) {

                gdi.pushX();
                gdi.fillRight();
                gdi.addInput("ComPortName", port, 10, 0, "COM-Port:");
                //gdi.addInput("BaudRate", "4800", 10, 0, "help:baudrate");
                gdi.fillDown();
                gdi.addCombo("BaudRate", 130, 100, 0, "help:baudrate");
                gdi.popX();
                gdi.addItem("BaudRate", "4800", 4800);
                gdi.addItem("BaudRate", "38400", 38400);
                gdi.selectItemByData("BaudRate", 38400);


                gdi.fillRight();
                gdi.addButton("SIPassive", "Lyssna...", SportIdentCB).setDefault();
                gdi.fillDown();
                gdi.addButton("Cancel", "Avbryt", SportIdentCB).setCancel();
                gdi.popX();
              }
            }
          }
          gdi.popX();
          gdi.dropLine();
          refillComPorts(gdi);
        }
        gdi.refresh();
      }
    }
    else if (bi.id=="SIInfo") {
      char bf[64];
      ListBoxInfo lbi;
      if (gdi.getSelectedItem("ComPort", lbi))
      {
        if (lbi.text.substr(0,3)=="TCP")
          sprintf_s(bf, 64, "TCP");
        else
          sprintf_s(bf, 64, "COM%d", lbi.data);

        gdi.addStringUT(0, lang.tl("H�mtar information om")+" "+string(bf)+".");
        gdi.addStringUT(0, gSI->getInfoString(bf));
        gdi.refresh();
      }
    }
    else if (bi.id=="AutoDetect")
    {
      gdi.addString("", 0, "S�ker efter SI-enheter... ");
      gdi.refresh();
      list<int> ports;
      if (!gSI->AutoDetect(ports)) {
        gdi.addString("SIInfo", 0, "help:5422");
        gdi.refresh();
        return 0;
      }
      char bf[128];
      gSI->CloseCom(0);

      while(!ports.empty()) {
        int p=ports.front();
        sprintf_s(bf, 128, "COM%d", p);

        gdi.addString((string("SIInfo")+bf).c_str(), 0, "#" + lang.tl("Startar SI p�") + " " + string(bf) + "...");
        gdi.refresh();
        if (gSI->OpenCom(bf)) {
          gSI->StartMonitorThread(bf);
          gdi.addStringUT(0, lang.tl("SI p�") + " " + string(bf) + ": " + lang.tl("OK"));
          gdi.addStringUT(0, gSI->getInfoString(bf));
          SI_StationInfo *si = gSI->findStation(bf);
          if (si && !si->Extended)
             gdi.addString("", boldText, "warn:notextended").setColor(colorDarkRed);
        }
        else if (gSI->OpenCom(bf)) {
          gSI->StartMonitorThread(bf);
          gdi.addStringUT(0, lang.tl("SI p�") + " " + string(bf) + ": " + lang.tl("OK"));
          gdi.addStringUT(0, gSI->getInfoString(bf));
          SI_StationInfo *si = gSI->findStation(bf);
          if (si && !si->Extended)
            gdi.addString("", boldText, "warn:notextended").setColor(colorDarkRed);
        }
        else gdi.addStringUT(0, lang.tl("SI p�") + " " + string(bf) + ": " +lang.tl("FEL, inget svar"));

        gdi.refresh();
        gdi.popX();
        gdi.dropLine();
        ports.pop_front();
      }
    }
    else if (bi.id == "PrinterSetup") {
      if (mode == ModeEntry) {
        printStartInfo = true;
        TabList::splitPrintSettings(*oe, gdi, true, TSITab, TabList::StartInfo);
      }
      else {
        printSplits = true;
        TabList::splitPrintSettings(*oe, gdi, true, TSITab, TabList::Splits);
      }
    }
    else if (bi.id == "AutoTie") {
      gEvent->setProperty("AutoTie", gdi.isChecked("AutoTie"));
    }
    else if (bi.id == "RentCardTie") {
      gEvent->setProperty("RentCard", gdi.isChecked(bi.id));
    }
    else if (bi.id == "TieOK") {
      tieCard(gdi);
    }
    else if (bi.id=="Interactive") {
      interactiveReadout=gdi.isChecked(bi.id);
      gEvent->setProperty("Interactive", interactiveReadout);

      if (mode == ModeAssignCards) {
        gdi.restore("ManualTie", false);
        showAssignCard(gdi, false);
      }
    }
    else if (bi.id=="Database") {
      useDatabase=gdi.isChecked(bi.id);
      gEvent->setProperty("Database", useDatabase);
    }
    else if (bi.id=="PrintSplits") {
      printSplits=gdi.isChecked(bi.id);
    }
    else if (bi.id=="StartInfo") {
      printStartInfo = gdi.isChecked(bi.id);
    }
    else if (bi.id == "UseManualInput") {
      manualInput = gdi.isChecked("UseManualInput");
      oe->setProperty("ManualInput", manualInput ? 1 : 0);
      gdi.restore("ManualInput");
      if (manualInput)
        showManualInput(gdi);
    }
    else if (bi.id=="Import") {
      int origin = bi.getExtraInt();

      vector< pair<string, string> > ext;
      ext.push_back(make_pair("Semikolonseparerad (csv)", "*.csv"));

      string file = gdi.browseForOpen(ext, "csv");
      if (!file.empty()) {
        gdi.restore("Help");
        csvparser csv;
        csv.importCards(*oe, file.c_str(), cards);
        if (cards.empty()) {
          csv.importPunches(*oe, file.c_str(), punches);
          if (!punches.empty()) {
            gdi.dropLine(2);
            gdi.addString("", 1, "Inl�sta st�mplar");
            set<string> dates;
            showReadPunches(gdi, punches, dates);

            filterDate.clear();
            filterDate.push_back(lang.tl("Inget filter"));
            for (set<string>::iterator it = dates.begin(); it!=dates.end(); ++it)
              filterDate.push_back(*it);

            gdi.dropLine(2);
            gdi.scrollToBottom();
            gdi.fillRight();
            gdi.pushX();
            gdi.addSelection("ControlType", 150, 300, 0, "Enhetstyp:");

            vector< pair<string, size_t> > d;
            oe->fillControlTypes(d);
            gdi.addItem("ControlType", d);
           // oe->fillControlTypes(gdi, "ControlType");
            gdi.selectItemByData("ControlType", oPunch::PunchCheck);

            gdi.addSelection("Filter", 150, 300, 0, "Datumfilter:");
            for (size_t k = 0; k<filterDate.size(); k++) {
              gdi.addItem("Filter", filterDate[k], k);
            }
            gdi.selectItemByData("Filter", 0);
            gdi.dropLine(1);
            gdi.addButton("SavePunches", "Spara", SportIdentCB).setExtra(origin);
            gdi.addButton("Cancel", "Avbryt", SportIdentCB).setExtra(origin);
            gdi.fillDown();
            gdi.popX();
          }
          else {
            loadPage(gdi);
            throw std::exception("Felaktigt filformat");
          }
        }
        else {
          gdi.pushX();
          gdi.dropLine(3);

          gdi.addString("", 1, "Inl�sta brickor");
          showReadCards(gdi, cards);
          gdi.dropLine();
          gdi.fillDown();
          if (interactiveReadout)
            gdi.addString("", 0, "V�lj Spara f�r att lagra brickorna. Interaktiv inl�sning �r aktiverad.");
          else
            gdi.addString("", 0, "V�lj Spara f�r att lagra brickorna. Interaktiv inl�sning �r INTE aktiverad.");

          gdi.fillRight();
          gdi.pushX();
          gdi.addButton("SaveCards", "Spara", SportIdentCB).setExtra(origin);
          gdi.addButton("Cancel", "Avbryt", SportIdentCB).setExtra(origin);
          gdi.fillDown();
          gdi.scrollToBottom();
        }
      }
    }
    else if (bi.id=="SavePunches") {
      int origin = bi.getExtraInt();
      ListBoxInfo lbi;
      gdi.getSelectedItem("ControlType", lbi);
      int type = lbi.data;
      gdi.getSelectedItem("Filter", lbi);
      bool dofilter = signed(lbi.data)>0;
      string filter = lbi.data < filterDate.size() ? filterDate[lbi.data] : "";

      gdi.restore("Help");
      for (size_t k=0;k<punches.size();k++) {
        if (dofilter && filter != punches[k].date)
          continue;
        oe->addFreePunch(punches[k].time, type, punches[k].card, true);
      }
      punches.clear();
      if (origin==1) {
        TabRunner &tc = dynamic_cast<TabRunner &>(*gdi.getTabs().get(TRunnerTab));
        tc.showInForestList(gdi);
      }
    }
    else if (bi.id=="SaveCards") {
      int origin = bi.getExtraInt();
      gdi.restore("Help");
      oe->synchronizeList(oLCardId, true, false);
      oe->synchronizeList(oLRunnerId, false, true);
      for (size_t k=0;k<cards.size();k++)
        insertSICard(gdi, cards[k]);

      oe->reEvaluateAll(set<int>(), true);
      cards.clear();
      if (origin==1) {
        TabRunner &tc = dynamic_cast<TabRunner &>(*gdi.getTabs().get(TRunnerTab));
        tc.showInForestList(gdi);
      }
    }
    else if (bi.id=="Save") {
      SICard sic;
      sic.clear(0);
      sic.CheckPunch.Code = -1;
      sic.CardNumber=gdi.getTextNo("SI");
      int f = convertAbsoluteTimeHMS(gdi.getText("Finish"), oe->getZeroTimeNum());
      int s = convertAbsoluteTimeHMS(gdi.getText("Start"), oe->getZeroTimeNum());
      sic.FinishPunch.Time= f % (24*3600);
      sic.StartPunch.Time = f % (24*3600);
      if (!gdi.isChecked("HasFinish")) {
        sic.FinishPunch.Code = -1;
        sic.FinishPunch.Time = 0;
      }

      if (!gdi.isChecked("HasStart")) {
        sic.StartPunch.Code = -1;
        sic.StartPunch.Time = 0;
      }

      double t = 0.1;
      for (sic.nPunch = 0; sic.nPunch<unsigned(NC); sic.nPunch++) {
        sic.Punch[sic.nPunch].Code=gdi.getTextNo("C" + itos(sic.nPunch+1));
        sic.Punch[sic.nPunch].Time=int(f*t+s*(1.0-t)) % (24*3600);
        t += ((1.0-t) * (sic.nPunch + 1) / 10.0) * ((rand() % 100) + 400.0)/500.0;
        if ((sic.nPunch%11) == 1 || 5 == (sic.nPunch%8))
          t += min(0.2, 0.9-t);
      }

      gdi.getRecorder().record("insertCard(" + itos(sic.CardNumber) + ", \"" + sic.serializePunches() + "\"); //Readout card");

      gSI->addCard(sic);
    }
    else if (bi.id=="SaveP") {
      SICard sic;
      sic.clear(0);
      sic.FinishPunch.Code = -1;
      sic.CheckPunch.Code = -1;
      sic.StartPunch.Code = -1;

      sic.CardNumber=gdi.getTextNo("SI");
      int f=convertAbsoluteTimeHMS(gdi.getText("Finish"), oe->getZeroTimeNum());
      if (f > 0) {
        sic.FinishPunch.Time = f;
        sic.FinishPunch.Code = 1;
        sic.PunchOnly = true;
        gSI->addCard(sic);
        return 0;
      }

      int s=convertAbsoluteTimeHMS(gdi.getText("Start"), oe->getZeroTimeNum());
      if (s > 0) {
        sic.StartPunch.Time = s;
        sic.StartPunch.Code = 1;
        sic.PunchOnly = true;
        gSI->addCard(sic);
        return 0;
      }

      sic.Punch[sic.nPunch].Code=gdi.getTextNo("C1");
      sic.Punch[sic.nPunch].Time=convertAbsoluteTimeHMS(gdi.getText("C2"), oe->getZeroTimeNum());
      sic.nPunch = 1;
      sic.PunchOnly = true;
      gSI->addCard(sic);
    }
    else if (bi.id=="Cancel") {
      int origin = bi.getExtraInt();
      activeSIC.clear(0);
      punches.clear();
      if (origin==1) {
        TabRunner &tc = dynamic_cast<TabRunner &>(*gdi.getTabs().get(TRunnerTab));
        tc.showInForestList(gdi);
        return 0;
      }
      loadPage(gdi);

      checkMoreCardsInQueue(gdi);
      return 0;
    }
    else if (bi.id=="OK1") {
      string name=gdi.getText("Runners");
      string club=gdi.getText("Club", true);

      if (name.length()==0){
        gdi.alert("Alla deltagare m�ste ha ett namn.");
        return 0;
      }

      pRunner r=0;
      DWORD rid;
      bool lookup = true;

      if (gdi.getData("RunnerId", rid) && rid>0) {
        r = gEvent->getRunner(rid, 0);

        if (r && r->getCard()) {
          if (!askOverwriteCard(gdi, r)) {
            r = 0;
            lookup = false;
          }
        }

        if (r && stringMatch(r->getName(), name)) {
          gdi.restore();
          //We have a match!
          SICard copy = activeSIC;
          activeSIC.clear(&activeSIC);
          processCard(gdi, r, copy);
          return 0;
        }
      }

      if (lookup) {
        r = gEvent->getRunnerByName(name, club);
        if (r && r->getCard()) {
          if (!askOverwriteCard(gdi, r))
            r = 0;
        }
      }

      if (r) {
        //We have a match!
        gdi.setData("RunnerId", r->getId());

        gdi.restore();
        SICard copy = activeSIC;
        activeSIC.clear(&activeSIC);
        processCard(gdi, r, copy);
        return 0;
      }

      //We have a new runner in our system
      gdi.fillRight();
      gdi.pushX();

      SICard si_copy=activeSIC;
      gEvent->convertTimes(si_copy);

      //Find matching class...
      vector<pClass> classes;
      int dist = gEvent->findBestClass(activeSIC, classes);

      if (classes.size()==1 && dist == 0 && si_copy.StartPunch.Time>0 && classes[0]->getType()!="tmp") {
        //We have a match!
        string club = gdi.getText("Club", true);

        if (club.length()==0 && oe->getMeOSFeatures().hasFeature(MeOSFeatures::Clubs))
          club=lang.tl("Klubbl�s");
        int year = 0;
        pRunner r=gEvent->addRunner(gdi.getText("Runners"), club,
                                    classes[0]->getId(), activeSIC.CardNumber, year, true);

        gdi.setData("RunnerId", r->getId());

        gdi.restore();
        SICard copy = activeSIC;
        activeSIC.clear(&activeSIC);
        processCard(gdi, r, copy);
        r->synchronize();
        return 0;
      }


      gdi.restore("restOK1", false);
      gdi.popX();
      gdi.dropLine(2);

      gdi.addInput("StartTime", gEvent->getAbsTime(si_copy.StartPunch.Time), 8, 0, "Starttid:");

      gdi.addSelection("Classes", 200, 300, 0, "Klass:");
      gEvent->fillClasses(gdi, "Classes", oEvent::extraNone, oEvent::filterNone);
      gdi.setInputFocus("Classes");

      if (classes.size()>0)
        gdi.selectItemByData("Classes", classes[0]->getId());

      gdi.dropLine();

      gdi.setRestorePoint("restOK2");

      gdi.addButton("Cancel", "Avbryt", SportIdentCB).setCancel();
      if (oe->getNumClasses() > 0)
        gdi.addButton("OK2", "OK", SportIdentCB).setDefault();
      gdi.fillDown();

      gdi.addButton("NewClass", "Skapa ny klass", SportIdentCB);

      gdi.popX();
      if (classes.size()>0)
        gdi.addString("FindMatch", 0, "Press Enter to continue").setColor(colorGreen);
      gdi.dropLine();

      gdi.refresh();
      return 0;
    }
    else if (bi.id=="OK2")
    {
      //New runner in existing class...

      ListBoxInfo lbi;
      gdi.getSelectedItem("Classes", lbi);

      if (lbi.data==0 || lbi.data==-1) {
        gdi.alert("Du m�ste v�lja en klass");
        return 0;
      }
      pClass pc = oe->getClass(lbi.data);
      if (pc && pc->getType()== "tmp")
        pc->setType("");

      string club = gdi.getText("Club", true);

      if (club.empty() && oe->getMeOSFeatures().hasFeature(MeOSFeatures::Clubs))
        club = lang.tl("Klubbl�s");

      int year = 0;
      pRunner r=gEvent->addRunner(gdi.getText("Runners"), club,
                                  lbi.data, activeSIC.CardNumber, year, true);

      r->setStartTimeS(gdi.getText("StartTime"));
      r->setCardNo(activeSIC.CardNumber, false);

      gdi.restore();
      SICard copy = activeSIC;
      activeSIC.clear(&activeSIC);
      processCard(gdi, r, copy);
    }
    else if (bi.id=="NewClass") {
      gdi.restore("restOK2", false);
      gdi.popX();
      gdi.dropLine(2);
      gdi.fillRight();
      gdi.pushX();

      gdi.addInput("ClassName", gEvent->getAutoClassName(), 10,0, "Klassnamn:");

      gdi.dropLine();
      gdi.addButton("Cancel", "Avbryt", SportIdentCB).setCancel();
      gdi.fillDown();
      gdi.addButton("OK3", "OK", SportIdentCB).setDefault();
      gdi.setInputFocus("ClassName", true);
      gdi.refresh();
      gdi.popX();
    }
    else if (bi.id=="OK3") {
      pCourse pc = 0;
      pClass pclass = 0;

      if (oe->getNumClasses() == 1 && oe->getClass(1) != 0 &&
              oe->getClass(1)->getType() == "tmp" && 
              oe->getClass(1)->getNumRunners(false, false, false) == 0) {
        pclass = oe->getClass(1);
        pclass->setType("");
        pclass->setName(gdi.getText("ClassName"));
        pc = pclass->getCourse();
        if (pc)
          pc->setName(gdi.getText("ClassName"));
      }

      if (pc == 0) {
        pc=gEvent->addCourse(gdi.getText("ClassName"));
        for(unsigned i=0;i<activeSIC.nPunch; i++)
          pc->addControl(activeSIC.Punch[i].Code);
      }
      if (pclass == 0) {
        pclass=gEvent->addClass(gdi.getText("ClassName"), pc->getId());
      }
      else
        pclass->setCourse(pc);
      int year = 0;
      pRunner r=gEvent->addRunner(gdi.getText("Runners"), gdi.getText("Club", true),
                                  pclass->getId(), activeSIC.CardNumber, year, true);

      r->setStartTimeS(gdi.getText("StartTime"));
      r->setCardNo(activeSIC.CardNumber, false);
      gdi.restore();
      SICard copy_sic = activeSIC;
      activeSIC.clear(&activeSIC);
      processCard(gdi, r, copy_sic);
    }
    else if (bi.id=="OK4") {
      //Existing runner in existing class...

      ListBoxInfo lbi;
      gdi.getSelectedItem("Classes", lbi);

      if (lbi.data==0 || lbi.data==-1)
      {
        gdi.alert("Du m�ste v�lja en klass");
        return 0;
      }

      DWORD rid;
      pRunner r;

      if (gdi.getData("RunnerId", rid) && rid>0)
        r = gEvent->getRunner(rid, 0);
      else r = gEvent->addRunner(lang.tl("Oparad bricka"), lang.tl("Ok�nd"), 0, 0, 0, false);

      r->setClassId(lbi.data, true);

      gdi.restore();
      SICard copy = activeSIC;
      activeSIC.clear(&activeSIC);
      processCard(gdi, r, copy);
    }
    else if (bi.id=="EntryOK") {
      storedInfo.clear();
      oe->synchronizeList(oLRunnerId, true, false);
      oe->synchronizeList(oLCardId, false, true);

      string name=gdi.getText("Name");
      if (name.empty()) {
        gdi.alert("Alla deltagare m�ste ha ett namn.");
        return 0;
      }
      int rid = bi.getExtraInt();
      pRunner r = oe->getRunner(rid, 0);
      int cardNo = gdi.getTextNo("CardNo");

      pRunner cardRunner = oe->getRunnerByCardNo(cardNo, 0, true);
      if (cardNo>0 && cardRunner!=0 && cardRunner!=r) {
        gdi.alert("Bricknummret �r upptaget (X).#" + cardRunner->getName() + ", " + cardRunner->getClass());
        return 0;
      }

      ListBoxInfo lbi;
      gdi.getSelectedItem("Class", lbi);

      if (signed(lbi.data)<=0) {
        pClass pc = oe->getClassCreate(0, lang.tl("�ppen klass"));
        lbi.data = pc->getId();
        pc->setAllowQuickEntry(true);
        pc->synchronize();
      }
      bool updated = false;
      int year = 0;

      if (r==0) {
        r = oe->addRunner(name, gdi.getText("Club", true), lbi.data, cardNo, year, true);
        r->setCardNo(0, false, false); // Clear to match below
      }
      else {
        int clubId = 0;
        if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Clubs)) {
          string cname = gdi.getText("Club", true);

          if (cname.empty()) {
            pClub club = oe->getClubCreate(0, cname);
            clubId = club->getId();
          }
        }
        int birthYear = 0;
        r->updateFromDB(name, clubId, lbi.data, cardNo, birthYear);
        r->setName(name, true);
        r->setClubId(clubId);
        r->setClassId(lbi.data, true);
        updated = true;
      }

      lastClubId=r->getClubId();
      lastClassId=r->getClassId();
      lastFee = gdi.getText("Fee", true);
      int lastFeeNum = oe->interpretCurrency(lastFee);

      r->setCardNo(cardNo, true);//XXX

      oDataInterface di=r->getDI();
      
      int cardFee = gdi.isChecked("RentCard") ? oe->getDI().getInt("CardFee") : 0;
      di.setInt("CardFee", cardFee);
      di.setInt("Fee", lastFeeNum);
      r->setFlag(oRunner::FlagFeeSpecified, true);
      
      writePayMode(gdi, lastFeeNum + (cardFee > 0 ? cardFee : 0), *r);

      di.setString("Phone", gdi.getText("Phone"));
      r->setFlag(oRunner::FlagTransferSpecified, gdi.hasField("AllStages"));
      r->setFlag(oRunner::FlagTransferNew, gdi.isChecked("AllStages"));
        
      r->setStartTimeS(gdi.getText("StartTime"));

      string bib = "";
      if (r->autoAssignBib())
        bib = ", " + lang.tl("Nummerlapp: ") + r->getBib();

      r->synchronize();

      gdi.restore("EntryLine");

      char bf[256];
      if (r->getClubId() != 0) {
        sprintf_s(bf, "(%d), %s, %s", r->getCardNo(), r->getClub().c_str(),
                    r->getClass().c_str());
      }
      else {
        sprintf_s(bf, "(%d), %s", r->getCardNo(), r->getClass().c_str());
      }

      string info(bf);
      if (r->getDI().getInt("CardFee") != 0)
        info+=lang.tl(", Hyrbricka");

      vector< pair<string, size_t> > modes;
      oe->getPayModes(modes);
      string pm;
      if (modes.size() > 1 &&  size_t(r->getPaymentMode()) < modes.size())
        pm = " (" + modes[r->getPaymentMode()].first + ")";
      if (r->getDI().getInt("Paid")>0)
        info += lang.tl(", Betalat") + pm;

      if (bib.length()>0)
        info+=bib;

      if (updated)
        info += lang.tl(" [Uppdaterad anm�lan]");

      gdi.pushX();
      gdi.fillRight();
      gdi.addString("ChRunner", 0, "#" + r->getName(), SportIdentCB).setColor(colorGreen).setExtra(r->getId());
      gdi.fillDown();
      gdi.addStringUT(0, info, 0);
      gdi.popX();

      generateStartInfo(gdi, *r);

      gdi.setRestorePoint("EntryLine");
      generateEntryLine(gdi, 0);
    }
    else if (bi.id=="EntryCancel") {
      gdi.restore("EntryLine");
      storedInfo.clear();
      generateEntryLine(gdi, 0);
    }
    else if (bi.id=="RentCard" || bi.id=="Paid" || bi.id == "AllStages") {
      updateEntryInfo(gdi);
    }
    else if (bi.id == "ManualOK") {
      if (runnerMatchedId == -1)
        throw meosException("L�paren hittades inte");

      bool useNow = gdi.getExtraInt("FinishTime") == 1;
      string time = useNow ? getLocalTimeOnly() : gdi.getText("FinishTime");

      int relTime = oe->getRelativeTime(time);
      if (relTime <= 0) {
        throw meosException("Ogiltig tid.");
      }
      bool ok = gdi.isChecked("StatusOK");
      bool dnf = gdi.isChecked("StatusDNF");

      pRunner r = oe->getRunner(runnerMatchedId, 0);
      if (r==0)
        throw meosException("L�paren hittades inte");

      if (r->getStatus() != StatusUnknown) {
        if (!gdi.ask("X har redan ett resultat. Vi du forts�tta?#" + r->getCompleteIdentification()))
          return 0;
      }

      gdi.restore("ManualInput", false);

      SICard sic;
      sic.runnerId = runnerMatchedId;
      sic.relativeFinishTime = relTime;
      sic.statusOK = ok;
      sic.statusDNF = dnf;

      gSI->addCard(sic);
    }
    else if (bi.id == "StatusOK") {
      bool ok = gdi.isChecked(bi.id);
      if (ok) {
       gdi.check("StatusDNF", false);
      }
    }
    else if (bi.id == "StatusDNF") {
      bool dnf = gdi.isChecked(bi.id);
      gdi.setInputStatus("StatusOK", !dnf);
      gdi.check("StatusOK", !dnf);
    }
    else if (bi.id == "CCSClear") {
      if (gdi.ask("Vill du g�ra om avbockningen fr�n b�rjan igen?")) {
        checkedCardFlags.clear();
        gdi.restore("CCSInit", false);
        showCheckCardStatus(gdi, "fillrunner");
        showCheckCardStatus(gdi, "stat");
        gdi.refresh();
      }
    }
    else if (bi.id == "CCSReport") {
      gdi.restore("CCSInit", false);
      showCheckCardStatus(gdi, "stat");
      showCheckCardStatus(gdi, "report");
      gdi.refresh();
    }
    else if (bi.id == "CCSPrint") {
      //gdi.print(oe);
      gdioutput gdiPrint("print", gdi.getScale(), gdi.getEncoding());
      gdiPrint.clearPage(false);
      
      int tCardPosX = cardPosX;
      int tCardPosY = cardPosY;
      int tCardOffsetX = cardOffsetX;
      int tCardCurrentCol = cardCurrentCol;

      showCheckCardStatus(gdiPrint, "stat");
      showCheckCardStatus(gdiPrint, "report");
      showCheckCardStatus(gdiPrint, "tickoff");
      gdiPrint.refresh();
      gdiPrint.print(oe);

      cardPosX = tCardPosX;
      cardPosY = tCardPosY;
      cardOffsetX = tCardOffsetX;
      cardCurrentCol = tCardCurrentCol;
    }
  }
  else if (type==GUI_LISTBOX) {
    ListBoxInfo bi=*(ListBoxInfo *)data;

    if (bi.id=="Runners") {
      pRunner r = gEvent->getRunner(bi.data, 0);
      if (r) {
        gdi.setData("RunnerId", bi.data);
        if (gdi.hasField("Club"))
          gdi.setText("Club", r->getClub());
        gdi.setText("FindMatch", lang.tl("Press Enter to continue"), true);
      }
    }
    else if (bi.id == "PayMode") {
      updateEntryInfo(gdi);
    }
    else if (bi.id=="ComPort") {
      char bf[64];

      if (bi.text.substr(0,3)!="TCP")
        sprintf_s(bf, 64, "COM%d", bi.data);
      else
        strcpy_s(bf, "TCP");

      if (gSI->IsPortOpen(bf))
        gdi.setText("StartSI", lang.tl("Koppla ifr�n"));
      else
        gdi.setText("StartSI", lang.tl("Aktivera"));
    }
    else if (bi.id=="ReadType") {
      gdi.restore("SIPageLoaded");
      mode = SIMode(bi.data);
      gdi.setInputStatus("StartInfo", mode == ModeEntry);
    
      if (mode==ModeAssignCards || mode==ModeEntry) {
        if (mode==ModeAssignCards) {
          gdi.dropLine(1);
          showAssignCard(gdi, true);
        }
        else {
          entryTips(gdi);
          generateEntryLine(gdi, 0);
        }
        gdi.setInputStatus("Interactive", mode == ModeAssignCards);
        gdi.setInputStatus("Database", mode != ModeAssignCards, true);
        gdi.disableInput("PrintSplits");
        
        gdi.disableInput("UseManualInput");
      }
      else if (mode==ModeReadOut) {
        gdi.enableInput("Interactive");
        gdi.enableInput("Database", true);
        gdi.enableInput("PrintSplits");
        gdi.enableInput("UseManualInput");
        gdi.fillDown();
        gdi.addButton("Import", "Importera fr�n fil...", SportIdentCB);

        if (gdi.isChecked("UseManualInput"))
          showManualInput(gdi);
      }
      else if (mode == ModeCardData) {
        showModeCardData(gdi);
      }
      else if (mode == ModeCheckCards) {
        showCheckCardStatus(gdi, "init");
      }
      gdi.refresh();
    }
    else if (bi.id=="Fee") {
      updateEntryInfo(gdi);
    }
    else if (bi.id == "NC") {
      NC = bi.data;
      PostMessage(gdi.getTarget(), WM_USER + 2, TSITab, 0);
    }
  }
  else if (type == GUI_LINK) {
    TextInfo ti = *(TextInfo *)data;
    if (ti.id == "ChRunner") {
      pRunner r = oe->getRunner(ti.getExtraInt(), 0);
      generateEntryLine(gdi, r);
    }
    else if (ti.id == "EditAssign") {
      int id = ti.getExtraInt();
      pRunner r = oe->getRunner(id, 0);
      if (r) {
        gdi.setText("CardNo", r->getCardNo());
        gdi.setText("RunnerId", r->getRaceIdentifier());
        gdi.setText("FindMatch", r->getCompleteIdentification(), true);
        runnerMatchedId = r->getId();
      }
    }
  }
  else if (type == GUI_COMBO) {
    ListBoxInfo bi=*(ListBoxInfo *)data;

    if (bi.id=="Fee") {
      updateEntryInfo(gdi);
    }
    else if (bi.id == "Runners") {
      DWORD rid;
      if ((gdi.getData("RunnerId", rid) && rid>0) || !gdi.getText("Club", true).empty())
        return 0; // Selected from list

      if (!bi.text.empty() && useDatabase) {
        pRunner db_r = oe->dbLookUpByName(bi.text, 0, 0, 0);
        if (!db_r && lastClubId)
          db_r = oe->dbLookUpByName(bi.text, lastClubId, 0, 0);

        if (db_r && gdi.hasField("Club")) {
          gdi.setText("Club", db_r->getClub());
        }
      }
      gdi.setText("FindMatch", lang.tl("Press Enter to continue"), true);

    }
  }
  else if (type == GUI_COMBOCHANGE) {
    ListBoxInfo bi=*(ListBoxInfo *)data;
    if (bi.id == "Runners") {
      inputId++;
      gdi.addTimeoutMilli(300, "AddRunnerInteractive", SportIdentCB).setExtra((void *)inputId);
    }
  }
  else if (type == GUI_EVENT) {
    EventInfo ev = *(EventInfo *)data;
    if (ev.id == "AutoComplete") {
      pRunner r = oe->getRunner(runnerMatchedId, 0);
      if (r) {
        gdi.setInputFocus("OK1");
        gdi.setText("Runners", r->getName());
        gdi.setData("RunnerId", runnerMatchedId);
        if (gdi.hasField("Club"))
          gdi.setText("Club", r->getClub());
        inputId = -1;
        gdi.setText("FindMatch", lang.tl("Press Enter to continue"), true);

      }
    }
  }
  else if (type == GUI_FOCUS) {
    InputInfo &ii=*(InputInfo *)data;

    if (ii.id == "FinishTime") {
      if (ii.getExtraInt() == 1) {
        ii.setExtra(0);
        ii.setFgColor(colorDefault);
        //gdi.refreshFast();
        gdi.setText(ii.id, "", true);
      }
    }
  }
  else if (type == GUI_TIMER) {
    TimerInfo &ti = *(TimerInfo *)(data);

    if (ti.id == "TieCard") {
      runnerMatchedId = ti.getExtraInt();
      tieCard(gdi);
      return 0;
    }

    if (inputId != ti.getExtraInt())
      return 0;

    if (ti.id == "RunnerId") {
      const string &text = gdi.getText(ti.id);
      int nr = atoi(text.c_str());

      pRunner r = 0;
      if (nr > 0) {
        r = getRunnerByIdentifier(nr);
        if (r == 0) {
          r = oe->getRunnerByBibOrStartNo(text, true);
          if (r == 0) {
            // Seek where a card is already defined
            r = oe->getRunnerByBibOrStartNo(text, false);
          }
        }
      }

      if (nr == 0 && text.size() > 2) {
        stdext::hash_set<int> f1, f2;
        r = oe->findRunner(text, 0, f1, f2);
      }
      if (r != 0) {
        gdi.setText("FindMatch", r->getCompleteIdentification(), true);
        runnerMatchedId = r->getId();
      }
      else {
        gdi.setText("FindMatch", "", true);
        runnerMatchedId = -1;
      }

      gdi.setInputStatus("TieOK", runnerMatchedId != -1);

      if (runnerMatchedId != -1 && gdi.getTextNo("CardNo") > 0 && gdi.isChecked("AutoTie"))
        tieCard(gdi);
    }
    else if (ti.id == "Manual") {
      const string &text = gdi.getText(ti.id);
      int nr = atoi(text.c_str());

      pRunner r = 0;
      if (nr > 0) {
        r = oe->getRunnerByBibOrStartNo(text, false);
        if (r == 0)
          r = oe->getRunnerByCardNo(nr, 0, true, true);
      }

      if (nr == 0 && text.size() > 2) {
        stdext::hash_set<int> f1, f2;
        r = oe->findRunner(text, 0, f1, f2);
      }
      if (r != 0) {
        gdi.setText("FindMatch", r->getCompleteIdentification(), true);
        runnerMatchedId = r->getId();
      }
      else {
        gdi.setText("FindMatch", "", true);
        runnerMatchedId = -1;
      }
    }
    else if (ti.id == "AddRunnerInteractive") {
      const string &text = gdi.getText("Runners");
      int nr = atoi(text.c_str());

      pRunner r = 0;
      if (nr > 0) {
        r = oe->getRunnerByBibOrStartNo(text, true);
      }

      if (nr == 0 && text.size() > 2) {
        stdext::hash_set<int> f1, f2;
        r = oe->findRunner(text, 0, f1, f2);
      }
      if (r != 0) {
        gdi.setText("FindMatch", lang.tl("X (press Ctrl+Space to confirm)#" + r->getCompleteIdentification()), true);
        runnerMatchedId = r->getId();
      }
      else {
        gdi.setText("FindMatch", "", true);
        runnerMatchedId = -1;
      }
    }
  }
  else if (type==GUI_INPUTCHANGE) {

    InputInfo ii=*(InputInfo *)data;
    if (ii.id == "RunnerId") {
      inputId++;
      gdi.addTimeoutMilli(300, ii.id, SportIdentCB).setExtra((void *)inputId);
    }
    else if (ii.id == "Manual") {
      inputId++;
      gdi.addTimeoutMilli(300, ii.id, SportIdentCB).setExtra((void *)inputId);
    }
    else if (ii.id == "CardNo" && mode == ModeAssignCards) {
      gdi.setInputStatus("TieOK", runnerMatchedId != -1);
    }
    else if (ii.id == "SI") {
      pRunner r = oe->getRunnerByCardNo(atoi(ii.text.c_str()), 0, true, false);
      if (r && r->getStartTime() > 0) {
        gdi.setText("Start", r->getStartTimeS());
        gdi.check("HasStart", false);
        int f = r->getStartTime() + 2800 + rand()%1200;
        gdi.setText("Finish", oe->getAbsTime(f));
        pCourse pc = r->getCourse(false);
        if (pc) {
          for (int n = 0; n < pc->getNumControls(); n++) {
            if (pc->getControl(n) && n < NC) {
              gdi.setText("C" + itos(n+1), pc->getControl(n)->getFirstNumber());
            }
          }
        }
      }
    }
  }
  else if (type==GUI_INPUT) {
    InputInfo &ii=*(InputInfo *)data;
    if (ii.id == "FinishTime") {
      if (ii.text.empty()) {
        ii.setExtra(1);
        ii.setFgColor(colorGreyBlue);
        gdi.setText(ii.id, lang.tl("Aktuell tid"), true);
      }
    }
    else if (ii.id=="CardNo") {
      int cardNo = gdi.getTextNo("CardNo");

      if (mode == ModeAssignCards) {
        if (runnerMatchedId != -1 && gdi.isChecked("AutoTie") && cardNo>0)
          gdi.addTimeoutMilli(50, "TieCard", SportIdentCB).setExtra((void *)runnerMatchedId);
      }
      else if (cardNo>0 && gdi.getText("Name").empty()) {
        SICard sic;
        sic.clear(0);
        sic.CardNumber = cardNo;

        entryCard(gdi, sic);
      }
    }
    else if (ii.id[0]=='*') {
      int si=atoi(ii.text.c_str());

      pRunner r=oe->getRunner(ii.getExtraInt(), 0);
      r->synchronize();

      if (r && r->getCardNo()!=si) {
        if (si==0 || !oe->checkCardUsed(gdi,*r, si)) {
          r->setCardNo(si, false);
          r->getDI().setInt("CardFee", oe->getDI().getInt("CardFee"));
          r->synchronize();
        }

        if (r->getCardNo())
          gdi.setText(ii.id, r->getCardNo());
        else
          gdi.setText(ii.id, "");
      }
    }
  }
  else if (type==GUI_INFOBOX) {
    DWORD loaded;
    if (!gdi.getData("SIPageLoaded", loaded))
      loadPage(gdi);
  }
  else if (type == GUI_CLEAR) {
    if (mode == ModeEntry) {
      storedInfo.clear();
      storedInfo.storedName = gdi.getText("Name");
      storedInfo.storedCardNo = gdi.getText("CardNo");
      storedInfo.storedClub = gdi.hasField("Club") ? gdi.getText("Club") : "";
      storedInfo.storedFee = gdi.getText("Fee", true);

      ListBoxInfo lbi;
      gdi.getSelectedItem("Class", lbi);
      storedInfo.storedClassId = lbi.data;
      storedInfo.storedPhone = gdi.getText("Phone");
      storedInfo.storedStartTime = gdi.getText("StartTime");
      
      storedInfo.allStages = gdi.isChecked("AllStages");
      storedInfo.rentState = gdi.isChecked("RentCard");
      storedInfo.hasPaid = gdi.isChecked("Paid");
      storedInfo.payMode = gdi.hasField("PayMode") ? gdi.getSelectedItem("PayMode").first : 0;
    }
    return 1;
  }

  return 0;
}


void TabSI::refillComPorts(gdioutput &gdi)
{
  if (!gSI) return;

  list<int> ports;
  gSI->EnumrateSerialPorts(ports);

  gdi.clearList("ComPort");
  ports.sort();
  char bf[256];
  int active=0;
  int inactive=0;
  while(!ports.empty())
  {
    int p=ports.front();
    sprintf_s(bf, 256, "COM%d", p);

    if (gSI->IsPortOpen(bf)){
      gdi.addItem("ComPort", string(bf)+" [OK]", p);
      active=p;
    }
    else{
      gdi.addItem("ComPort", bf, p);
      inactive=p;
    }

    ports.pop_front();
  }

  if (gSI->IsPortOpen("TCP"))
    gdi.addItem("ComPort", "TCP [OK]");
  else
    gdi.addItem("ComPort", "TCP");

  if (active){
    gdi.selectItemByData("ComPort", active);
    gdi.setText("StartSI", lang.tl("Koppla ifr�n"));
  }
  else{
    gdi.selectItemByData("ComPort", inactive);
    gdi.setText("StartSI", lang.tl("Aktivera"));
  }
}

void TabSI::showReadPunches(gdioutput &gdi, vector<PunchInfo> &punches, set<string> &dates)
{
  char bf[64];
  int yp = gdi.getCY();
  int xp = gdi.getCX();
  dates.clear();
  for (size_t k=0;k<punches.size(); k++) {
    sprintf_s(bf, "%d.", k+1);
    gdi.addStringUT(yp, xp, 0, bf);

    pRunner r = oe->getRunnerByCardNo(punches[k].card, punches[k].time);
    sprintf_s(bf, "%d", punches[k].card);
    gdi.addStringUT(yp, xp+40, 0, bf, 240);

    if (r!=0)
      gdi.addStringUT(yp, xp+100, 0, r->getName(), 170);

    if (punches[k].date[0] != 0) {
      gdi.addStringUT(yp, xp+280, 0, punches[k].date, 75);
      dates.insert(punches[k].date);
    }
    if (punches[k].time>0)
      gdi.addStringUT(yp, xp+360, 0, oe->getAbsTime(punches[k].time));
    else
      gdi.addStringUT(yp, xp+360, 0, MakeDash("-"));

    yp += gdi.getLineHeight();
  }
}

void TabSI::showReadCards(gdioutput &gdi, vector<SICard> &cards)
{
  char bf[64];
  int yp = gdi.getCY();
  int xp = gdi.getCX();
  for (size_t k=0;k<cards.size(); k++) {
    sprintf_s(bf, "%d.", k+1);
    gdi.addStringUT(yp, xp, 0, bf);

    pRunner r = oe->getRunnerByCardNo(cards[k].CardNumber, 0);
    sprintf_s(bf, "%d", cards[k].CardNumber);
    gdi.addStringUT(yp, xp+40, 0, bf, 240);

    if (r!=0)
      gdi.addStringUT(yp, xp+100, 0, r->getName(), 240);

    gdi.addStringUT(yp, xp+300, 0, oe->getAbsTime(cards[k].FinishPunch.Time));
    yp += gdi.getLineHeight();
  }
}

SportIdent &TabSI::getSI(const gdioutput &gdi) {
  if (!gSI) {
    HWND hWnd=gdi.getMain();
    gSI = new SportIdent(hWnd, 0);
    gSI->SetZeroTime(gEvent->getZeroTimeNum());
  }
  return *gSI;
}

bool TabSI::loadPage(gdioutput &gdi) {
  gdi.clearPage(true);
  gdi.pushX();
  gdi.selectTab(tabId);
  oe->checkDB();
  gdi.setData("SIPageLoaded", 1);

  if (!gSI) {
    getSI(gdi);
    if (oe->isClient())
      interactiveReadout = false;
  }
#ifdef _DEBUG
  gdi.fillRight();
  gdi.pushX();
  gdi.addInput("SI", "", 10, SportIdentCB, "SI");
  int s = 3600+(rand()%60)*60;
  int f = s + 1800 + rand()%900;
  
  gdi.setCX(gdi.getCX()+gdi.getLineHeight());
  
  gdi.dropLine(1.4);
  gdi.addCheckbox("HasStart", "");
  gdi.dropLine(-1.4);
  gdi.setCX(gdi.getCX()-gdi.getLineHeight());
  gdi.addInput("Start", oe->getAbsTime(s), 6, 0, "Start");
  
  gdi.dropLine(1.4);
  gdi.addCheckbox("HasFinish", "");
  gdi.dropLine(-1.4);
  gdi.setCX(gdi.getCX()-gdi.getLineHeight());

  gdi.addInput("Finish", oe->getAbsTime(f), 6, 0, "M�l");
  gdi.addSelection("NC", 45, 200, SportIdentCB, "NC");
  const int src[11] = {33, 34, 45, 50, 36, 38, 59, 61, 62, 67, 100};
  
  for (int i = 0; i < 32; i++)
    gdi.addItem("NC", itos(i), i);

  gdi.selectItemByData("NC", NC);

  for (int i = 0; i < NC; i++) {
    int level = min(i, NC-i)/5;
    int c;
    if (i < NC /2) {
      int ix = i%6;
      c = src[ix] + level * 10;
      if (c == 100)
        c = 183;
    }
    else {
      int ix = 10-(NC-i-1)%5;
      c = src[ix] + level * 10;
    }

    gdi.addInput("C" + itos(i+1), itos(c), 3, 0, "#C" + itos(i+1));
  }
  /*
  gdi.addInput("C1", "33", 5, 0, "#C1");
  gdi.addInput("C2", "34", 5, 0, "#C2");
  gdi.addInput("C3", "45", 5, 0, "#C3");
  gdi.addInput("C4", "50", 5, 0, "#C4");
  gdi.addInput("C5", "61", 5, 0, "#C5");
  gdi.addInput("C6", "62", 5, 0, "#C6");
  gdi.addInput("C7", "67", 5, 0, "#C7");

  gdi.addInput("C8", "100", 5, 0, "#C8");
  */
  
  gdi.dropLine();
  gdi.addButton("Save", "Bricka", SportIdentCB);
  gdi.fillDown();

  gdi.addButton("SaveP", "St�mpling", SportIdentCB);
  gdi.popX();
#endif
  gdi.addString("", boldLarge, "SportIdent");
  gdi.dropLine();

  gdi.pushX();
  gdi.fillRight();
  gdi.addSelection("ComPort", 120, 200, SportIdentCB);
  gdi.addButton("StartSI", "#Aktivera+++", SportIdentCB);
  gdi.addButton("SIInfo", "Info", SportIdentCB);

  refillComPorts(gdi);

  gdi.addButton("AutoDetect", "S�k och starta automatiskt...", SportIdentCB);
  gdi.addButton("PrinterSetup", "Skrivarinst�llningar...", SportIdentCB, "Skrivarinst�llningar f�r str�cktider och startbevis");

  gdi.popX();
  gdi.fillDown();
  gdi.dropLine(2.2);

  int xb = gdi.getCX();
  int yb = gdi.getCY();

  gdi.fillRight();
  if (!oe->empty()) {
    gdi.setCX(xb + gdi.scaleLength(10));
    gdi.setCY(yb + gdi.scaleLength(10));
    gdi.addString("", fontMediumPlus, "Funktion:");
    gdi.addSelection("ReadType", 200, 200, SportIdentCB);
    gdi.addItem("ReadType", lang.tl("Avl�sning/radiotider"), ModeReadOut);
    gdi.addItem("ReadType", lang.tl("Tilldela hyrbrickor"), ModeAssignCards);
    gdi.addItem("ReadType", lang.tl("Avst�mning hyrbrickor"), ModeCheckCards);
    gdi.addItem("ReadType", lang.tl("Anm�lningsl�ge"), ModeEntry);
    gdi.addItem("ReadType", lang.tl("Print Card Data"), ModeCardData);

    gdi.selectItemByData("ReadType", mode);
    gdi.dropLine(2.5);
    gdi.setCX(xb + gdi.scaleLength(10));
  }
  else {
    mode = ModeCardData;
  }

  if (!oe->empty())
    gdi.addCheckbox("Interactive", "Interaktiv inl�sning", SportIdentCB, interactiveReadout);

  if (oe->empty() || oe->useRunnerDb())
    gdi.addCheckbox("Database", "Anv�nd l�pardatabasen", SportIdentCB, useDatabase);

  gdi.addCheckbox("PrintSplits", "Str�cktidsutskrift[check]", SportIdentCB, printSplits);
  
  if (!oe->empty()) {
    gdi.addCheckbox("StartInfo", "Startbevis", SportIdentCB, printStartInfo, "Skriv ut startbevis f�r deltagaren");
    if (mode != ModeEntry)
      gdi.disableInput("StartInfo");
  }
  if (!oe->empty())
    gdi.addCheckbox("UseManualInput", "Manuell inmatning", SportIdentCB, manualInput);

  gdi.fillDown();

  if (!oe->empty()) {
    RECT rc = {xb, yb, gdi.getWidth(), gdi.getHeight()};
    gdi.addRectangle(rc, colorLightBlue);
  }
  gdi.popX();
  gdi.dropLine(2);
  gdi.setRestorePoint("SIPageLoaded");

  if (mode==ModeReadOut) {
    gdi.addButton("Import", "Importera fr�n fil...", SportIdentCB);

    gdi.setRestorePoint("Help");
    gdi.addString("", 10, "help:471101");

    if (gdi.isChecked("UseManualInput"))
      showManualInput(gdi);

    gdi.dropLine();
  }
  else if (mode==ModeAssignCards) {
    gdi.dropLine(1);
    showAssignCard(gdi, true);
  }
  else if (mode == ModeEntry) {
    entryTips(gdi);
    generateEntryLine(gdi, 0);
    gdi.disableInput("Interactive");
    gdi.disableInput("PrintSplits");
    gdi.disableInput("UseManualInput");
  }
  else if (mode == ModeCardData) {
    showModeCardData(gdi);
  }
  else if (mode == ModeCheckCards) {
    showCheckCardStatus(gdi, "init");
  }


  // Unconditional clear
  activeSIC.clear(0);

  checkMoreCardsInQueue(gdi);
  gdi.refresh();
  return true;
}

void InsertSICard(gdioutput &gdi, SICard &sic)
{
  TabSI &tsi = dynamic_cast<TabSI &>(*gdi.getTabs().get(TSITab));
  tsi.insertSICard(gdi, sic);
}

pRunner TabSI::autoMatch(const SICard &sic, pRunner db_r)
{
  assert(useDatabase);
  //Look up in database.
  if (!db_r)
    db_r = gEvent->dbLookUpByCard(sic.CardNumber);

  pRunner r=0;

  if (db_r) {
    r = gEvent->getRunnerByName(db_r->getName(), db_r->getClub());

    if ( !r ) {
      vector<pClass> classes;
      int dist = gEvent->findBestClass(sic, classes);

      if (classes.size()==1 && dist>=-1 && dist<=1) { //Almost perfect match found. Assume it is it!
        r = gEvent->addRunnerFromDB(db_r, classes[0]->getId(), true);
        r->setCardNo(sic.CardNumber, false);
      }
      else r=0; //Do not assume too much...
    }
  }
  if (r && r->getCard()==0)
    return r;
  else return 0;
}

void TabSI::insertSICard(gdioutput &gdi, SICard &sic)
{
  string msg;
  try {
    insertSICardAux(gdi, sic);
  }
  catch(std::exception &ex) {
    msg = ex.what();
  }
  catch(...) {
    msg = "Ett ok�nt fel intr�ffade.";
  }

  if (!msg.empty())
    gdi.alert(msg);
}

void TabSI::insertSICardAux(gdioutput &gdi, SICard &sic)
{
  if (oe->isReadOnly()) {
    gdi.makeEvent("ReadCard", "insertSICard", sic.CardNumber, 0, true);
    return;
  }

  DWORD loaded;
  bool pageLoaded=gdi.getData("SIPageLoaded", loaded);

  if (pageLoaded && manualInput)
    gdi.restore("ManualInput");

  if (!pageLoaded && !insertCardNumberField.empty()) {
    if (gdi.insertText(insertCardNumberField, itos(sic.CardNumber)))
      return;
  }

  if (mode==ModeAssignCards) {
    if (!pageLoaded) {
      CardQueue.push_back(sic);
      gdi.addInfoBox("SIREAD", "Inl�st bricka st�lld i k�");
    }
    else assignCard(gdi, sic);
    return;
  }
  else if (mode==ModeEntry) {
    if (!pageLoaded) {
      CardQueue.push_back(sic);
      gdi.addInfoBox("SIREAD", "Inl�st bricka st�lld i k�");
    }
    else entryCard(gdi, sic);
    return;
  }
  if (mode==ModeCheckCards) {
    if (!pageLoaded) {
      CardQueue.push_back(sic);
      gdi.addInfoBox("SIREAD", "Inl�st bricka st�lld i k�");
    }
    else 
      checkCard(gdi, sic, true);
    return;
  }
  else if (mode == ModeCardData) {
    savedCards.push_back(make_pair(savedCardUniqueId++, sic));
    
    if (printSplits) {
      generateSplits(savedCards.back().first, gdi);
    }
    if (savedCards.size() > 1 && pageLoaded) {
      RECT rc = {30, gdi.getCY(), gdi.scaleLength(250), gdi.getCY() + 3};
      gdi.addRectangle(rc);
    }

    if (pageLoaded) {
      gdi.enableInput("CreateCompetition", true);
      printCard(gdi, savedCards.back().first, false);
      gdi.dropLine();
      gdi.refreshFast();
      gdi.scrollToBottom();
    }
    return;
  }
  gEvent->synchronizeList(oLCardId, true, false);
  gEvent->synchronizeList(oLRunnerId, false, true);

  if (sic.PunchOnly) {
    processPunchOnly(gdi, sic);
    return;
  }
  pRunner r;
  if (sic.runnerId == 0)
    r = gEvent->getRunnerByCardNo(sic.CardNumber, 0, false);
  else {
    r = gEvent->getRunner(sic.runnerId, 0);
    sic.CardNumber = r->getCardNo();
  }

  bool readBefore = sic.runnerId == 0 ? gEvent->isCardRead(sic) : false;

  bool sameCardNewRace = !readBefore && r && r->getCard();

  if (!pageLoaded) {
    if (sic.runnerId != 0)
      throw meosException("Internal error");
    //SIPage not loaded...

    if (!r && useDatabase)
      r=autoMatch(sic, 0);

    // Assign a class if not already done
    autoAssignClass(r, sic);

    if (interactiveReadout) {
      if (r && r->getClassId() && !readBefore && !sameCardNewRace) {
        //We can do a silent read-out...
        processCard(gdi, r, sic, true);
        return;
      }
      else {
        CardQueue.push_back(sic);
        gdi.addInfoBox("SIREAD", "info:readout_action#" + gEvent->getCurrentTimeS()+"#"+itos(sic.CardNumber), 0, SportIdentCB);
        return;
      }
    }
    else {
      if (!readBefore) {
        if (r && r->getClassId() && !sameCardNewRace)
          processCard(gdi, r, sic, true);
        else
          processUnmatched(gdi, sic, true);
      }
      else
        gdi.addInfoBox("SIREAD", "Brickan redan inl�st.", 0, SportIdentCB);
    }
    return;
  }
  else if (activeSIC.CardNumber) {
    //We are already in interactive mode...

    // Assign a class if not already done
    autoAssignClass(r, sic);

    if (r && r->getClassId() && !readBefore && !sameCardNewRace) {
      //We can do a silent read-out...
      processCard(gdi, r, sic, true);
      return;
    }

    string name;
    if (r)
      name = " ("+r->getName()+")";

    //char bf[256];
    //sprintf_s(bf, 256, "SI-%d inl�st%s.\nBrickan har st�llts i k�.", sic.CardNumber, name.c_str());
    name = itos(sic.CardNumber) + name;
    CardQueue.push_back(sic);
    //gdi.addInfoBox("SIREAD", gEvent->getCurrentTimeS()+": "+bf);
    gdi.addInfoBox("SIREAD", "info:readout_queue#" + gEvent->getCurrentTimeS()+ "#" + name);
    return;
  }

  if (readBefore) {
    //We stop processing of new cards, while working...
    // Thus cannot be in interactive mode
    activeSIC=sic;
    char bf[256];

    if (interactiveReadout) {
      sprintf_s(bf, 256,  "SI X �r redan inl�st. Ska den l�sas in igen?#%d", sic.CardNumber);

      if (!gdi.ask(bf)) {
        if (printSplits) {
          pRunner runner = oe->getRunnerByCardNo(sic.CardNumber, 0);
          if (runner)
            generateSplits(runner, gdi);
        }
        activeSIC.clear(0);
        if (manualInput)
          showManualInput(gdi);
        checkMoreCardsInQueue(gdi);
        return;
      }
    }
    else {
      if (printSplits) {
        pRunner runner = oe->getRunnerByCardNo(sic.CardNumber, 0);
        if (runner)
          generateSplits(runner, gdi);
      }

      gdi.dropLine();
      sprintf_s(bf, 256,  "SI X �r redan inl�st. Anv�nd interaktiv inl�sning om du vill l�sa brickan igen.#%d", sic.CardNumber);
      gdi.addString("", 0, bf).setColor(colorRed);
      gdi.dropLine();
      gdi.scrollToBottom();
      gdi.refresh();
      activeSIC.clear(0);
      checkMoreCardsInQueue(gdi);
      return;
    }
  }

  pRunner db_r = 0;
  if (sic.runnerId == 0) {
    r = gEvent->getRunnerByCardNo(sic.CardNumber, 0, !readBefore);

    if (!r && useDatabase) {
      //Look up in database.
      db_r = gEvent->dbLookUpByCard(sic.CardNumber);
      if (db_r)
        r = autoMatch(sic, db_r);
    }
  }

  // If there is no class, auto create
  if (interactiveReadout && oe->getNumClasses()==0) {
    gdi.fillDown();
    gdi.dropLine();
    gdi.addString("", 1, "Skapar saknad klass").setColor(colorGreen);
    gdi.dropLine();
    pCourse pc=gEvent->addCourse(lang.tl("Ok�nd klass"));
    for(unsigned i=0;i<sic.nPunch; i++)
      pc->addControl(sic.Punch[i].Code);
    gEvent->addClass(lang.tl("Ok�nd klass"), pc->getId())->setType("tmp");
  }

  // Assign a class if not already done
  autoAssignClass(r, sic);

  if (r && r->getClassId() && !r->getCard()) {
    SICard copy = sic;
    activeSIC.clear(0);
    processCard(gdi, r, copy); //Everyting is OK
    if (gdi.isChecked("UseManualInput"))
      showManualInput(gdi);
  }
  else {
    if (interactiveReadout) {
      startInteractive(gdi, sic, r, db_r);
    }
    else {
      SICard copy = sic;
      activeSIC.clear(0);
      processUnmatched(gdi, sic, !pageLoaded);
    }
  }
}

void TabSI::startInteractive(gdioutput &gdi, const SICard &sic, pRunner r, pRunner db_r)
{
  if (!r) {
    gdi.setRestorePoint();
    gdi.fillDown();
    gdi.dropLine();
    char bf[256];
    sprintf_s(bf, 256, "SI X inl�st. Brickan �r inte knuten till n�gon l�pare (i skogen).#%d", sic.CardNumber);

    gdi.dropLine();
    gdi.addString("", 1, bf);
    gdi.dropLine();
    gdi.fillRight();
    gdi.pushX();

    gdi.addCombo("Runners", 200, 300, SportIdentCB, "Namn:");
    gEvent->fillRunners(gdi, "Runners", false, oEvent::RunnerFilterOnlyNoResult);

    if (db_r){
      gdi.setText("Runners", db_r->getName()); //Data from DB
    }
    else if (sic.FirstName[0] || sic.LastName[0]){ //Data from SI-card
      gdi.setText("Runners", string(sic.FirstName)+" "+sic.LastName);
    }
    if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Clubs)) {
      gdi.addCombo("Club", 200, 300, 0, "Klubb:");
      gEvent->fillClubs(gdi, "Club");

      if (db_r)
        gdi.setText("Club", db_r->getClub()); //Data from DB
    }
    if (gdi.getText("Runners").empty() || !gdi.hasField("Club"))
      gdi.setInputFocus("Runners");
    else
      gdi.setInputFocus("Club");

    //Process this card.
    activeSIC=sic;
    gdi.dropLine();
    gdi.setRestorePoint("restOK1");
    gdi.addButton("OK1", "OK", SportIdentCB).setDefault();
    gdi.fillDown();
    gdi.addButton("Cancel", "Avbryt", SportIdentCB).setCancel();
    gdi.popX();
    gdi.addString("FindMatch", 0, "").setColor(colorGreen);
    gdi.registerEvent("AutoComplete", SportIdentCB).setKeyCommand(KC_AUTOCOMPLETE);
    gdi.dropLine();
    gdi.scrollToBottom();
    gdi.refresh();
  }
  else {
    //Process this card.
    activeSIC=sic;

    //No class. Select...
    gdi.setRestorePoint();

    char bf[256];
    sprintf_s(bf, 256, "SI X inl�st. Brickan tillh�r Y som saknar klass.#%d#%s",
              sic.CardNumber, r->getName().c_str());

    gdi.dropLine();
    gdi.addString("", 1, bf);

    gdi.fillRight();
    gdi.pushX();

    gdi.addSelection("Classes", 200, 300, 0, "Klass:");
    gEvent->fillClasses(gdi, "Classes", oEvent::extraNone, oEvent::filterNone);
    gdi.setInputFocus("Classes");
    //Find matching class...
    vector<pClass> classes;
    gEvent->findBestClass(sic, classes);
    if (classes.size() > 0)
      gdi.selectItemByData("Classes", classes[0]->getId());

    gdi.dropLine();

    gdi.addButton("OK4", "OK", SportIdentCB).setDefault();
    gdi.fillDown();

    gdi.popX();
    gdi.setData("RunnerId", r->getId());
    gdi.scrollToBottom();
    gdi.refresh();
  }
}

// Insert card without converting times and with/without runner
void TabSI::processInsertCard(const SICard &sic)
{
  if (oe->isCardRead(sic))
    return;

  pRunner runner = oe->getRunnerByCardNo(sic.CardNumber, 0, true);
  pCard card = oe->allocateCard(runner);
  card->setReadId(sic);
  card->setCardNo(sic.CardNumber);

  if (sic.CheckPunch.Code!=-1)
    card->addPunch(oPunch::PunchCheck, sic.CheckPunch.Time, 0);

  if (sic.StartPunch.Code!=-1)
    card->addPunch(oPunch::PunchStart, sic.StartPunch.Time, 0);

  for(unsigned i=0;i<sic.nPunch;i++)
    card->addPunch(sic.Punch[i].Code, sic.Punch[i].Time, 0);

  if (sic.FinishPunch.Code!=-1)
    card->addPunch(oPunch::PunchFinish, sic.FinishPunch.Time,0 );

  //Update to SQL-source
  card->synchronize();

  if (runner) {
    vector<int> mp;
    runner->addPunches(card, mp);
  }
}

bool TabSI::processUnmatched(gdioutput &gdi, const SICard &csic, bool silent)
{
  SICard sic(csic);
  pCard card=gEvent->allocateCard(0);

  card->setReadId(csic);
  card->setCardNo(csic.CardNumber);

  char bf[16];
  _itoa_s(sic.CardNumber, bf, 16, 10);
  string cardno(bf);

  string info=lang.tl("Ok�nd bricka ") + cardno + ".";
  string warnings;

  // Write read card to log
  logCard(sic);

  // Convert punch times to relative times.
  gEvent->convertTimes(sic);

  if (sic.CheckPunch.Code!=-1)
    card->addPunch(oPunch::PunchCheck, sic.CheckPunch.Time, 0);

  if (sic.StartPunch.Code!=-1)
    card->addPunch(oPunch::PunchStart, sic.StartPunch.Time, 0);

  for(unsigned i=0;i<sic.nPunch;i++)
    card->addPunch(sic.Punch[i].Code, sic.Punch[i].Time, 0);

  if (sic.FinishPunch.Code!=-1)
    card->addPunch(oPunch::PunchFinish, sic.FinishPunch.Time, 0);
  else
    warnings+=lang.tl("M�lst�mpling saknas.");

  //Update to SQL-source
  card->synchronize();

  RECT rc;
  rc.left=15;
  rc.right=gdi.getWidth()-10;
  rc.top=gdi.getCY()+gdi.getLineHeight()-5;
  rc.bottom=rc.top+gdi.getLineHeight()*2+14;

  if (!silent) {
    gdi.fillDown();
    //gdi.dropLine();
    gdi.addRectangle(rc, colorLightRed, true);
    gdi.addStringUT(rc.top+6, rc.left+20, 1, info);
    //gdi.dropLine();
    if (gdi.isChecked("UseManualInput"))
      showManualInput(gdi);

    gdi.scrollToBottom();
  }
  else {
    gdi.addInfoBox("SIINFO", "#" + info, 10000);
  }
  gdi.makeEvent("DataUpdate", "sireadout", 0, 0, true);

  checkMoreCardsInQueue(gdi);
  return true;
}

void TabSI::rentCardInfo(gdioutput &gdi, int width)
{
  RECT rc;
  rc.left=15;
  rc.right=rc.left+width;
  rc.top=gdi.getCY()-7;
  rc.bottom=rc.top+gdi.getLineHeight()+5;

  gdi.addRectangle(rc, colorYellow, true);
  gdi.addString("", rc.top+2, rc.left+width/2, 1|textCenter, "V�nligen �terl�mna hyrbrickan.");
}

bool TabSI::processCard(gdioutput &gdi, pRunner runner, const SICard &csic, bool silent)
{
  if (!runner)
    return false;
  if (runner->getClubId())
    lastClubId = runner->getClubId();

  runner = runner->getMatchedRunner(csic);

  int lh=gdi.getLineHeight();
  //Update from SQL-source
  runner->synchronize();

  if (!runner->getClassId())
    runner->setClassId(gEvent->addClass(lang.tl("Ok�nd klass"))->getId(), true);

  // Choose course from pool
  pClass cls=gEvent->getClass(runner->getClassId());
  if (cls && cls->hasCoursePool()) {
    unsigned leg=runner->legToRun();

    if (leg<cls->getNumStages()) {
      pCourse c = cls->selectCourseFromPool(leg, csic);
      if (c)
        runner->setCourseId(c->getId());
    }
  }

  if (cls && cls->hasUnorderedLegs()) {
    pCourse crs = cls->selectParallelCourse(*runner, csic);
    if (crs) {
      runner->setCourseId(crs->getId());
      runner->synchronize(true);
    }
  }

  pClass pclass=gEvent->getClass(runner->getClassId());
  if (!runner->getCourse(false) && !csic.isManualInput()) {

    if (pclass && !pclass->hasMultiCourse() && !pclass->hasDirectResult()) {
      pCourse pcourse=gEvent->addCourse(runner->getClass());
      pclass->setCourse(pcourse);

      for(unsigned i=0;i<csic.nPunch; i++)
        pcourse->addControl(csic.Punch[i].Code);

      char msg[256];

      sprintf_s(msg, lang.tl("Skapade en bana f�r klassen %s med %d kontroller fr�n brickdata (SI-%d)").c_str(),
          pclass->getName().c_str(), csic.nPunch, csic.CardNumber);

      if (silent)
        gdi.addInfoBox("SIINFO", string("#") + msg, 15000);
      else
        gdi.addStringUT(0, msg);
    }
    else {
      if (!(pclass && pclass->hasDirectResult())) {
        const char *msg="L�pare saknar klass eller bana";

        if (silent)
          gdi.addInfoBox("SIINFO", msg, 15000);
        else
          gdi.addString("", 0, msg);
      }
    }
  }

  pCourse pcourse=runner->getCourse(false);

  if (pcourse)
    pcourse->synchronize();
  else if (pclass && pclass->hasDirectResult())
    runner->setStatus(StatusOK, true, false, false);
  //silent=true;
  SICard sic(csic);
  string info, warnings, cardno;
  vector<int> MP;

  if (!csic.isManualInput()) {
    pCard card=gEvent->allocateCard(runner);

    card->setReadId(csic);
    card->setCardNo(sic.CardNumber);

    char bf[16];
    _itoa_s(sic.CardNumber, bf, 16, 10);
    cardno = bf;

    info=runner->getName() + " (" + cardno + "),   " + runner->getClub() + ",   " + runner->getClass();

    // Write read card to log
    logCard(sic);

    // Convert punch times to relative times.
    oe->convertTimes(sic);
    pCourse prelCourse = runner->getCourse(false);
    const int finishPT = prelCourse ? prelCourse->getFinishPunchType() : oPunch::PunchFinish;
    bool hasFinish = false;

    if (sic.CheckPunch.Code!=-1)
      card->addPunch(oPunch::PunchCheck, sic.CheckPunch.Time,0);

    if (sic.StartPunch.Code!=-1)
      card->addPunch(oPunch::PunchStart, sic.StartPunch.Time,0);

    for(unsigned i=0;i<sic.nPunch;i++) {
      if (sic.Punch[i].Code == finishPT)
        hasFinish = true;
      card->addPunch(sic.Punch[i].Code, sic.Punch[i].Time,0);
    }
    if (sic.FinishPunch.Code!=-1) {
      card->addPunch(oPunch::PunchFinish, sic.FinishPunch.Time,0);
      if (finishPT == oPunch::PunchFinish)
        hasFinish = true;
    }

    if (!hasFinish)
      warnings+=lang.tl("M�lst�mpling saknas.");

    card->synchronize();
    runner->addPunches(card, MP);
    runner->hasManuallyUpdatedTimeStatus();
  }
  else {
    //Manual input
    info = runner->getName() + ",   " + runner->getClub() + ",   " + runner->getClass();
    runner->setCard(0);

    if (csic.statusOK) {
      runner->setStatus(StatusOK, true, false);
      runner->setFinishTime(csic.relativeFinishTime);
    }
    else if (csic.statusDNF) {
      runner->setStatus(StatusDNF, true, false);
      runner->setFinishTime(0);
    }
    else {
      runner->setStatus(StatusMP, true, false);
      runner->setFinishTime(csic.relativeFinishTime);
    }

    cardno = MakeDash("-");
    runner->evaluateCard(true, MP, false, false);
    runner->hasManuallyUpdatedTimeStatus();
  }

  //Update to SQL-source
  runner->synchronize();

  RECT rc;
  rc.left=15;
  rc.right=gdi.getWidth()-10;
  rc.top=gdi.getCY()+gdi.getLineHeight()-5;
  rc.bottom=rc.top+gdi.getLineHeight()*2+14;

  if (!warnings.empty())
    rc.bottom+=gdi.getLineHeight();

  if (runner->getStatus()==StatusOK) {
    gEvent->calculateResults(oEvent::RTClassResult);
    if (runner->getTeam())
      gEvent->calculateTeamResults(runner->getLegNumber(), false);
    string placeS = runner->getTeam() ? runner->getTeam()->getLegPlaceS(runner->getLegNumber(), false) : runner->getPlaceS();

    if (!silent) {
      gdi.fillDown();
      //gdi.dropLine();
      gdi.addRectangle(rc, colorLightGreen, true);

      gdi.addStringUT(rc.top+6, rc.left+20, 1, info);
      if (!warnings.empty())
        gdi.addStringUT(rc.top+6+2*lh, rc.left+20, 0, warnings);

      string statusline = lang.tl("Status OK,    ") +
                          lang.tl("Tid: ") + runner->getRunningTimeS() +
                          lang.tl(",      Prel. placering: ") + placeS;


      statusline += lang.tl(",     Prel. bomtid: ") + runner->getMissedTimeS();
      gdi.addStringUT(rc.top+6+lh, rc.left+20, 0, statusline);

      if (runner->getDI().getInt("CardFee") != 0)
        rentCardInfo(gdi, rc.right-rc.left);
      gdi.scrollToBottom();
    }
    else {
      string msg="#" + runner->getName()  + " (" + cardno + ")\n"+
          runner->getClub()+". "+runner->getClass() +
          "\n" + lang.tl("Tid:  ") + runner->getRunningTimeS() + lang.tl(", Plats  ") + placeS;

      gdi.addInfoBox("SIINFO", msg, 10000);
    }
  }
  else {
    string msg=lang.tl("Status")+": "+ lang.tl(runner->getStatusS());

    if (!MP.empty()) {
      msg=msg+", (";
      vector<int>::iterator it;
      char bf[32];

      for(it=MP.begin(); it!=MP.end(); ++it){
        _itoa_s(*it, bf, 32, 10);
        msg=msg+bf+" ";
      }
      msg+=" "+lang.tl("saknas")+".)";
    }

    if (!silent) {
      gdi.fillDown();
      gdi.dropLine();
      gdi.addRectangle(rc, colorLightRed, true);

      gdi.addStringUT(rc.top+6, rc.left+20, 1, info);
      if (!warnings.empty())
        gdi.addStringUT(rc.top+6+lh*2, rc.left+20, 1, warnings);

      gdi.addStringUT(rc.top+6+lh, rc.left+20, 0, msg);

      if (runner->getDI().getInt("CardFee") != 0)
        rentCardInfo(gdi, rc.right-rc.left);

      gdi.scrollToBottom();
    }
    else {
      string statusmsg="#" + runner->getName()  + " (" + cardno + ")\n"+
          runner->getClub()+". "+runner->getClass() +
          "\n" + msg;

      gdi.addInfoBox("SIINFO", statusmsg, 10000);
    }
  }

  tabForceSync(gdi, gEvent);
  gdi.makeEvent("DataUpdate", "sireadout", runner ? runner->getId() : 0, 0, true);

  // Print splits
  if (printSplits)
    generateSplits(runner, gdi);

  activeSIC.clear(&csic);

  checkMoreCardsInQueue(gdi);
  return true;
}

void TabSI::processPunchOnly(gdioutput &gdi, const SICard &csic)
{
  SICard sic=csic;
  DWORD loaded;
  gEvent->convertTimes(sic);
  oFreePunch *ofp=0;

  if (sic.nPunch==1)
    ofp=gEvent->addFreePunch(sic.Punch[0].Time, sic.Punch[0].Code, sic.CardNumber, true);
  else if (sic.FinishPunch.Time > 0)
    ofp=gEvent->addFreePunch(sic.FinishPunch.Time, oPunch::PunchFinish, sic.CardNumber, true);
  else if (sic.StartPunch.Time > 0)
    ofp=gEvent->addFreePunch(sic.StartPunch.Time, oPunch::PunchStart, sic.CardNumber, true);
  else
    ofp=gEvent->addFreePunch(sic.CheckPunch.Time, oPunch::PunchCheck, sic.CardNumber, true);

  if (ofp) {
    pRunner r = ofp->getTiedRunner();
    if (gdi.getData("SIPageLoaded", loaded)){
      //gEvent->getRunnerByCard(sic.CardNumber);

      if (r) {
        string str=r->getName() + lang.tl(" st�mplade vid ") + ofp->getSimpleString();
        gdi.addStringUT(0, str);
        gdi.dropLine();
      }
      else {
        string str="SI " + itos(sic.CardNumber) + lang.tl(" (ok�nd) st�mplade vid ") + ofp->getSimpleString();
        gdi.addStringUT(0, str);
        gdi.dropLine(0.3);
      }
      gdi.scrollToBottom();
    }

    tabForceSync(gdi, gEvent);
    gdi.makeEvent("DataUpdate", "sireadout", r ? r->getId() : 0, 0, true);

  }

  checkMoreCardsInQueue(gdi);
  return;
}


void TabSI::entryCard(gdioutput &gdi, const SICard &sic)
{
  gdi.setText("CardNo", sic.CardNumber);

  string name;
  string club;
  if (useDatabase) {
    pRunner db_r=oe->dbLookUpByCard(sic.CardNumber);

    if (db_r) {
      name=db_r->getName();
      club=db_r->getClub();
    }
  }

  //Else get name from card
  if (name.empty() && (sic.FirstName[0] || sic.LastName[0]))
    name=string(sic.FirstName)+" "+sic.LastName;

  gdi.setText("Name", name);
  if (gdi.hasField("Club"))
    gdi.setText("Club", club);

  if (name.empty())
    gdi.setInputFocus("Name");
  else if (club.empty() && gdi.hasField("Club"))
    gdi.setInputFocus("Club");
  else
    gdi.setInputFocus("Class");
}

void TabSI::assignCard(gdioutput &gdi, const SICard &sic)
{

  if (interactiveReadout) {
    pRunner rb = oe->getRunner(runnerMatchedId, 0);

    if (rb && oe->checkCardUsed(gdi, *rb, sic.CardNumber))
      return;

    gdi.setText("CardNo", sic.CardNumber);
    if (runnerMatchedId != -1 && gdi.isChecked("AutoTie"))
      tieCard(gdi);
    return;
  }

  int storedAssigneIndex = currentAssignIndex;
  //Try first current focus
  BaseInfo *ii=gdi.getInputFocus();
  char sicode[32];
  sprintf_s(sicode, "%d", sic.CardNumber);

  if (ii && ii->id[0]=='*') {
    currentAssignIndex=atoi(ii->id.c_str()+1);
  }
  else { //If not correct focus, use internal counter
    char id[32];
    sprintf_s(id, "*%d", currentAssignIndex++);

    ii=gdi.setInputFocus(id);

    if (!ii) {
      currentAssignIndex=0;
      sprintf_s(id, "*%d", currentAssignIndex++);
      ii=gdi.setInputFocus(id);
    }
  }

  if (ii && ii->getExtraInt()) {
    pRunner r=oe->getRunner(ii->getExtraInt(), 0);
    if (r) {
      if (oe->checkCardUsed(gdi, *r, sic.CardNumber)) {
        currentAssignIndex = storedAssigneIndex;
        return;
      }
      if (r->getCardNo()==0 ||
                gdi.ask("Skriv �ver existerande bricknummer?")) {

        r->setCardNo(sic.CardNumber, false);
        r->getDI().setInt("CardFee", oe->getDI().getInt("CardFee"));
        r->synchronize();
        gdi.setText(ii->id, sicode);
      }
    }
    gdi.TabFocus();
  }

  checkMoreCardsInQueue(gdi);
}

void TabSI::generateEntryLine(gdioutput &gdi, pRunner r)
{
  oe->synchronizeList(oLRunnerId, true, false);
  oe->synchronizeList(oLCardId, false, true);

  gdi.restore("EntryLine", false);
  gdi.setRestorePoint("EntryLine");
  gdi.dropLine(1);
  int xb = gdi.getCX();
  int yb = gdi.getCY();
  gdi.dropLine();
  gdi.setCX(xb + gdi.scaleLength(10));

  gdi.fillRight();

  gdi.pushX();
  storedInfo.checkAge();
  gdi.addInput("CardNo", storedInfo.storedCardNo, 8, SportIdentCB, "Bricka:");

  gdi.addInput("Name", storedInfo.storedName, 16, 0, "Namn:");

  if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Clubs)) {
    gdi.addCombo("Club", 180, 200, 0, "Klubb:", "Skriv f�rsta bokstaven i klubbens namn och tryck pil-ner f�r att leta efter klubben");
    oe->fillClubs(gdi, "Club");
    if (storedInfo.storedClub.empty())
      gdi.selectItemByData("Club", lastClubId);
    else
      gdi.setText("Club", storedInfo.storedClub);
  }

  gdi.addSelection("Class", 150, 200, 0, "Klass:");
  oe->fillClasses(gdi, "Class", oEvent::extraNumMaps, oEvent::filterOnlyDirect);
  if (storedInfo.storedClassId > 0 && gdi.selectItemByData("Class", storedInfo.storedClassId)) {
  }
  else if (!gdi.selectItemByData("Class", lastClassId)) {
    gdi.selectFirstItem("Class");
  }

  if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Economy)) {
    gdi.addCombo("Fee", 60, 150, SportIdentCB, "Anm. avgift:");
    oe->fillFees(gdi, "Fee", false);
    
    if (!storedInfo.storedFee.empty() && storedInfo.storedFee != "@")
      gdi.setText("Fee", storedInfo.storedFee);
    else
      gdi.setText("Fee", lastFee);

    gdi.dropLine(1.2);
    generatePayModeWidget(gdi);
    gdi.dropLine(-1.2);
  }

  gdi.popX();
  gdi.dropLine(3.1);

  gdi.addString("",0, "Starttid:");
  gdi.dropLine(-0.2);
  gdi.addInput("StartTime", storedInfo.storedStartTime, 5, 0, "");

  gdi.setCX(gdi.getCX()+gdi.scaleLength(50));
  gdi.dropLine(0.2);
  gdi.setCX(gdi.getCX()+gdi.scaleLength(5));

  gdi.addString("", 0, "Telefon:");
  gdi.dropLine(-0.2);
  gdi.addInput("Phone", storedInfo.storedPhone, 12, 0, "");
  gdi.dropLine(0.2);

  gdi.setCX(gdi.getCX()+gdi.scaleLength(50));

  gdi.addCheckbox("RentCard", "Hyrbricka", SportIdentCB, storedInfo.rentState);
  if (oe->hasNextStage())
    gdi.addCheckbox("AllStages", "Anm�l till efterf�ljande etapper", SportIdentCB, storedInfo.allStages);
      
  if (r!=0) {
    if (r->getCardNo()>0)
      gdi.setText("CardNo", r->getCardNo());

    gdi.setText("Name", r->getName());
    if (gdi.hasField("Club")) {
      gdi.selectItemByData("Club", r->getClubId());
    }
    gdi.selectItemByData("Class", r->getClassId());

    oDataConstInterface dci = r->getDCI();
    if (gdi.hasField("Fee"))
      gdi.setText("Fee", oe->formatCurrency(dci.getInt("Fee")));

    gdi.setText("Phone", dci.getString("Phone"));

    gdi.check("RentCard", dci.getInt("CardFee") != 0);
    if (gdi.hasField("Paid"))
      gdi.check("Paid", dci.getInt("Paid")>0);
    else if (gdi.hasField("PayMode")) {
      int paidId = dci.getInt("Paid") > 0 ? r->getPaymentMode() : 1000;
      gdi.selectItemByData("PayMode", paidId);
    }

    if (gdi.hasField("AllStages")) {
      gdi.check("AllStages", r->hasFlag(oRunner::FlagTransferNew));
    }
  }

  gdi.popX();
  gdi.dropLine(2);
  gdi.addButton("EntryOK", "OK", SportIdentCB).setDefault().setExtra(r ? r->getId() : 0);
  gdi.addButton("EntryCancel", "Avbryt", SportIdentCB).setCancel();
  gdi.dropLine(0.1);
  gdi.addString("EntryInfo", fontMediumPlus, "").setColor(colorDarkRed);
  updateEntryInfo(gdi);
  gdi.setInputFocus("CardNo");
  gdi.dropLine(2);
  
  RECT rc = {xb, yb, gdi.getWidth(), gdi.getHeight()};
  gdi.addRectangle(rc, colorLightCyan);
  gdi.scrollToBottom();
  gdi.popX();
  gdi.setOnClearCb(SportIdentCB);
}

void TabSI::updateEntryInfo(gdioutput &gdi)
{
  int fee = oe->interpretCurrency(gdi.getText("Fee", true));
  if (gdi.isChecked("RentCard")) {
    int cardFee = oe->getDI().getInt("CardFee");
    if (cardFee > 0)
      fee += cardFee;
  }
  if (gdi.isChecked("AllStages")) {
    int nums = oe->getNumStages();
    int cs = oe->getStageNumber();
    if (nums > 0 && cs <= nums) {
      int np = nums - cs + 1;
      fee *= np;
    }

  }

  string method;
  if (oe->getMeOSFeatures().hasFeature(MeOSFeatures::Economy)) {
    bool invoice = true; 
    if (gdi.hasField("PayMode")) {
      invoice = gdi.getSelectedItem("PayMode").first == 1000;
    }
    else
      invoice = !gdi.isChecked("Paid");

    if (!invoice)
      method = lang.tl("Att betala");
    else
      method = lang.tl("Faktureras");

    gdi.setText("EntryInfo", lang.tl("X: Y. Tryck <Enter> f�r att spara#" +
                        method + "#" + oe->formatCurrency(fee)), true);
  }
  else {
    gdi.setText("EntryInfo", lang.tl("Press Enter to continue"), true);

  }
}

void TabSI::generateSplits(const pRunner r, gdioutput &gdi)
{
  gdioutput gdiprint(2.0, gdi.getEncoding(), gdi.getHWND(), splitPrinter);
  vector<int> mp;
  r->evaluateCard(true, mp);
  r->printSplits(gdiprint);
  gdiprint.print(splitPrinter, oe, false, true);
}

void TabSI::generateStartInfo(gdioutput &gdi, const oRunner &r) {
  if (printStartInfo) {
    gdioutput gdiprint(2.0, gdi.getEncoding(), gdi.getHWND(), splitPrinter);
    r.printStartInfo(gdiprint);
    gdiprint.print(splitPrinter, oe, false, true);
  }
}

void TabSI::printerSetup(gdioutput &gdi)
{
  gdi.printSetup(splitPrinter);
}

void TabSI::checkMoreCardsInQueue(gdioutput &gdi) {
  // Create a local list to avoid stack overflow
  list<SICard> cards = CardQueue;
  CardQueue.clear();
  std::exception storedEx;
  bool fail = false;

  while (!cards.empty()) {
    SICard c = cards.front();
    cards.pop_front();
    try {
      gdi.RemoveFirstInfoBox("SIREAD");
      insertSICard(gdi, c);
    }
    catch (std::exception &ex) {
      fail = true;
      storedEx = ex;
    }
  }

  if (fail)
    throw storedEx;
}

bool TabSI::autoAssignClass(pRunner r, const SICard &sic) {
  if (r && r->getClassId()==0) {
    vector<pClass> classes;
    int dist = oe->findBestClass(sic, classes);

    if (classes.size() == 1 && dist>=-1 && dist<=1) // Allow at most one wrong punch
      r->setClassId(classes[0]->getId(), true);
  }

  return r && r->getClassId() != 0;
}

void TabSI::showManualInput(gdioutput &gdi) {
  runnerMatchedId = -1;
  gdi.setRestorePoint("ManualInput");
  gdi.fillDown();
  gdi.dropLine(0.7);

  int x = gdi.getCX();
  int y = gdi.getCY();

  gdi.setCX(x+gdi.scaleLength(15));
  gdi.dropLine();
  gdi.addString("", 1, "Manuell inmatning");
  gdi.fillRight();
  gdi.pushX();
  gdi.dropLine();
  gdi.addInput("Manual", "", 20, SportIdentCB, "Nummerlapp, SI eller Namn:");
  gdi.addInput("FinishTime", lang.tl("Aktuell tid"), 8, SportIdentCB, "M�ltid:").setFgColor(colorGreyBlue).setExtra(1);
  gdi.dropLine(1.2);
  gdi.addCheckbox("StatusOK", "Godk�nd", SportIdentCB, true);
  gdi.addCheckbox("StatusDNF", "Utg�tt", SportIdentCB, false);
  gdi.dropLine(-0.3);
  gdi.addButton("ManualOK", "OK", SportIdentCB).setDefault();
  gdi.fillDown();
  gdi.dropLine(2);
  gdi.popX();
  gdi.addString("FindMatch", 0, "", 0).setColor(colorDarkGreen);
  gdi.dropLine();

  RECT rc;
  rc.left=x;
  rc.right=gdi.getWidth()-10;
  rc.top=y;
  rc.bottom=gdi.getCY()+gdi.scaleLength(5);
  gdi.dropLine();
  gdi.addRectangle(rc, colorLightBlue);
  //gdi.refresh();
  gdi.scrollToBottom();
}

void TabSI::tieCard(gdioutput &gdi) {
  int card = gdi.getTextNo("CardNo");
  pRunner r = oe->getRunner(runnerMatchedId, 0);

  if (r == 0)
    throw meosException("Invalid binding");

  if (oe->checkCardUsed(gdi, *r, card))
    return;

  if (r->getCardNo() > 0 && r->getCardNo() != card) {
    if (!gdi.ask("X har redan bricknummer Y. Vill du �ndra det?#" + r->getName() + "#" + itos(r->getCardNo())))
      return;
  }

  bool rent = gdi.isChecked("RentCardTie");
  r->setCardNo(card, true, false);
  r->getDI().setInt("CardFee", rent ? oe->getDI().getInt("CardFee") : 0);
  r->synchronize(true);

  gdi.restore("ManualTie");
  gdi.pushX();
  gdi.fillRight();
  gdi.addStringUT(italicText, getLocalTimeOnly());
  if (!r->getBib().empty())
    gdi.addStringUT(0, r->getBib(), 0);
  gdi.addStringUT(0, r->getName(), 0);

  if (r->getTeam() && r->getTeam()->getName() != r->getName())
    gdi.addStringUT(0, "(" + r->getTeam()->getName() + ")", 0);
  else if (!r->getClub().empty())
    gdi.addStringUT(0, "(" + r->getClub() + ")", 0);

  gdi.addStringUT(1, itos(r->getCardNo()), 0).setColor(colorDarkGreen);
  gdi.addString("EditAssign", 0, "�ndra", SportIdentCB).setExtra(r->getId());
  gdi.dropLine(1.5);
  gdi.popX();

  showAssignCard(gdi, false);
}

void TabSI::showAssignCard(gdioutput &gdi, bool showHelp) {
  gdi.enableInput("Interactive");
  gdi.disableInput("Database", true);
  gdi.disableInput("PrintSplits");
  gdi.disableInput("StartInfo");
  gdi.disableInput("UseManualInput");
  gdi.setRestorePoint("ManualTie");
  gdi.fillDown();
  if (interactiveReadout) {
    if (showHelp)
      gdi.addString("", 10, "Avmarkera 'X' f�r att hantera alla bricktildelningar samtidigt.#" + lang.tl("Interaktiv inl�sning"));
  }
  else {
    if (showHelp)
      gdi.addString("", 10, "Markera 'X' f�r att hantera deltagarna en och en.#" + lang.tl("Interaktiv inl�sning"));
    gEvent->assignCardInteractive(gdi, SportIdentCB);
    gdi.refresh();
    return;
  }

  runnerMatchedId = -1;
  gdi.fillDown();
  gdi.dropLine(0.7);

  int x = gdi.getCX();
  int y = gdi.getCY();

  gdi.setCX(x+gdi.scaleLength(15));
  gdi.dropLine();
  gdi.addString("", 1, "Knyt bricka / deltagare");
  gdi.fillRight();
  gdi.pushX();
  gdi.dropLine();
  gdi.addInput("RunnerId", "", 20, SportIdentCB, "Nummerlapp, lopp-id eller namn:");
  gdi.addInput("CardNo", "", 8, SportIdentCB, "Bricknr:");
  gdi.dropLine(1.2);
  gdi.addCheckbox("AutoTie", "Knyt automatiskt efter inl�sning", SportIdentCB, oe->getPropertyInt("AutoTie", 1) != 0);
  gdi.addCheckbox("RentCardTie", "Hyrd", SportIdentCB, oe->getPropertyInt("RentCard", 0) != 0);

  gdi.dropLine(-0.3);
  gdi.addButton("TieOK", "OK", SportIdentCB).setDefault();
  gdi.disableInput("TieOK");
  gdi.setInputFocus("RunnerId");
  gdi.fillDown();
  gdi.dropLine(2);
  gdi.popX();
  gdi.addString("FindMatch", 0, "", 0).setColor(colorDarkGreen);
  gdi.dropLine();

  RECT rc;
  rc.left=x;
  rc.right=gdi.getWidth()+gdi.scaleLength(5);
  rc.top=y;
  rc.bottom=gdi.getCY()+gdi.scaleLength(5);
  gdi.dropLine();
  gdi.addRectangle(rc, colorLightBlue);
  gdi.scrollToBottom();
}

pRunner TabSI::getRunnerByIdentifier(int identifier) const {
  int id;
  if (identifierToRunnerId.lookup(identifier, id)) {
    pRunner r = oe->getRunner(id, 0);
    if (r && r->getRaceIdentifier() == identifier)
      return r;
    else
      minRunnerId = 0; // Map is out-of-date
  }

  if (identifier < minRunnerId)
    return 0;

  minRunnerId = MAXINT;
  identifierToRunnerId.clear();

  pRunner ret = 0;
  vector<pRunner> runners;
  oe->autoSynchronizeLists(false);
  oe->getRunners(0, 0, runners, false);
  for ( size_t k = 0; k< runners.size(); k++) {
    if (runners[k]->getRaceNo() == 0) {
      int i = runners[k]->getRaceIdentifier();
      identifierToRunnerId.insert(i, runners[k]->getId());
      minRunnerId = min(minRunnerId, i);
      if (i == identifier)
        ret = runners[k];
    }
  }
  return ret;
}

bool TabSI::askOverwriteCard(gdioutput &gdi, pRunner r) const {
  return gdi.ask("ask:overwriteresult#" + r->getCompleteIdentification());
}

void TabSI::showModeCardData(gdioutput &gdi) {
  gdi.disableInput("Interactive", true);
  gdi.enableInput("Database", true);
  gdi.enableInput("PrintSplits");
  gdi.disableInput("StartInfo", true);
  gdi.disableInput("UseManualInput", true);

  gdi.dropLine();
  gdi.fillDown();
  gdi.pushX();
  gdi.addString("", boldLarge,  "Print Card Data");
  gdi.addString("", 10, "help:analyzecard");
  gdi.dropLine();
  gdi.fillRight();
  gdi.addButton("ClearMemory", "Clear Memory", SportIdentCB);
  gdi.addButton("SaveMemory", "Spara...", SportIdentCB);
  if (oe->empty()) {
    gdi.addButton("CreateCompetition", "Create Competition", SportIdentCB);
    if (savedCards.empty())
      gdi.disableInput("CreateCompetition");
  }
  gdi.dropLine(3);
  gdi.popX();
  bool first = true;
  for (list<pair<int, SICard> >::iterator it = savedCards.begin(); it != savedCards.end(); ++it) {
    gdi.dropLine(0.5);
    if (!first) {
      RECT rc = {30, gdi.getCY(), gdi.scaleLength(250), gdi.getCY() + 3};
      gdi.addRectangle(rc);
    }
    first = false;

    printCard(gdi, it->first, false);
  }
}

void TabSI::EditCardData::handle(gdioutput &gdi, BaseInfo &info, GuiEventType type) {
  if (type == GUI_LINK) {
    TextInfo &ti = dynamic_cast<TextInfo &>(info);
    int cardId = ti.getExtraInt();
    SICard &card = tabSI->getCard(cardId);
    ti.id = "card" + itos(cardId);
    gdi.removeControl("CardName");
    gdi.removeControl("ClubName");
    gdi.removeControl("OKCard");
    gdi.removeControl("CancelCard");

    string name, club;
    if (card.FirstName[0])
      name = card.FirstName + (card.LastName[0] ? (" " + string(card.LastName)) : "");
    club = card.Club;
    bool noName = name.empty();
    bool noClub = club.empty();
    if (noName)
      name = lang.tl("Namn");
    if (noClub)
      club = lang.tl("Klubb");

    InputInfo &ii = gdi.addInput(ti.xp-2, ti.yp-2, "CardName", name, 18, 0);
    ii.setHandler(this);
    InputInfo &ii2 = gdi.addInput(ti.xp + ii.getWidth(), ti.yp-2, "ClubName", club, 22, 0);
    ii2.setExtra(noClub).setHandler(this);
    ButtonInfo &bi = gdi.addButton(ii2.getX() + 2 + ii2.getWidth(), ti.yp-4, "OKCard", "OK", 0);
    bi.setExtra(cardId).setHandler(this);
    bi.setDefault();
    int w, h;
    bi.getDimension(gdi, w, h);
    gdi.addButton(bi.xp + w + 4, ti.yp-4, "CancelCard", "Avbryt", 0).setCancel().setHandler(this);
    gdi.setInputFocus(ii.id, noName);
  }
  else if (type == GUI_BUTTON) {
    ButtonInfo bi = dynamic_cast<ButtonInfo &>(info);
    //OKCard or CancelCard
    if (bi.id == "OKCard") {
      int cardId = bi.getExtraInt();
      SICard &card = tabSI->getCard(cardId);
      string name = gdi.getText("CardName");
      string club = gdi.getBaseInfo("ClubName").getExtra() ? "" : gdi.getText("ClubName");
      string given = getGivenName(name);
      string familty = getFamilyName(name);
      strncpy_s(card.FirstName, given.c_str(), sizeof(card.FirstName)-1);
      strncpy_s(card.LastName, familty.c_str(), sizeof(card.LastName)-1);
      strncpy_s(card.Club, club.c_str(), sizeof(card.Club)-1);
  
      string s = name;
      if (!club.empty())
        s += ", " + club;
      gdi.setText("card" + itos(cardId), s, true);
    }

    gdi.removeControl("CardName");
    gdi.removeControl("ClubName");
    gdi.removeControl("OKCard");
    gdi.removeControl("CancelCard");
  }
  else if (type == GUI_FOCUS) {
    InputInfo &ii = dynamic_cast<InputInfo &>(info);
    if (ii.getExtraInt()) {
      ii.setExtra(0);
      gdi.setInputFocus(ii.id, true);
    }
  }
}


void TabSI::printCard(gdioutput &gdi, int cardId, bool forPrinter) const {
  SICard &c = getCard(cardId);
  if (c.readOutTime[0] == 0)
    strcpy_s(c.readOutTime, getLocalTime().c_str());

  gdi.pushX();
  gdi.fillRight();
  string name, clubName;
  if (c.FirstName[0] != 0) {
    name = string(c.FirstName) + " " + c.LastName;
    clubName = c.Club;
  }
  else {
    const RunnerDBEntry *r = oe->getRunnerDatabase().getRunnerByCard(c.CardNumber);
    if (r) {
      r->getName(name);
      const oClub *club = oe->getRunnerDatabase().getClub(r->clubNo);
      if (club) {
        clubName = club->getName();
        strncpy_s(c.Club, clubName.c_str(), sizeof(c.Club)-1);
      }
      string given = r->getGivenName();
      string family = r->getFamilyName();
      strncpy_s(c.FirstName, given.c_str(), sizeof(c.FirstName)-1);
      strncpy_s(c.LastName, family.c_str(), sizeof(c.LastName)-1);
    }
  }

  gdi.addString("", 1, "Bricka X#" + itos(c.CardNumber));

  if (!forPrinter && name.empty())
    name = lang.tl("Ok�nd");

  if (!name.empty()) {
    if (!clubName.empty())
      name += ", "  + clubName;
    gdi.fillDown();
    gdi.addStringUT(0, name).setExtra(cardId).setHandler(&editCardData);
    gdi.popX();
  }
  gdi.fillDown();
  gdi.addStringUT(0, c.readOutTime);
  gdi.popX();

  int start = NOTIME;
  if (c.CheckPunch.Code != -1)
    gdi.addString("", 0, "Check: X#" + formatTimeHMS(c.CheckPunch.Time));

  if (c.StartPunch.Code != -1) {
    gdi.addString("", 0, "Start: X#" + formatTimeHMS(c.StartPunch.Time));
    start = c.StartPunch.Time;
  }
  int xp = gdi.getCX();
  int xp2 = xp + gdi.scaleLength(25);
  int xp3 = xp2 + gdi.scaleLength(35);
  int xp4 = xp3 + gdi.scaleLength(60);
  int xp5 = xp4 + gdi.scaleLength(45);

  int accTime = 0;
  int days = 0;
  for (unsigned k = 0; k < c.nPunch; k++) {
    int cy = gdi.getCY();
    gdi.addStringUT(cy, xp, 0, itos(k+1) + ".");
    gdi.addStringUT(cy, xp2, 0, itos(c.Punch[k].Code));
    gdi.addStringUT(cy, xp3, 0, formatTimeHMS(c.Punch[k].Time % (24*3600)));
    if (start != NOTIME) {
      int legTime = analyzePunch(c.Punch[k], start, accTime, days);
      if (legTime > 0)
        gdi.addStringUT(cy, xp5-gdi.scaleLength(10), textRight, formatTime(legTime));

      gdi.addStringUT(cy, xp5 + gdi.scaleLength(40), textRight, formatTime(days*3600*24 + accTime));
    }
    else {
      start = c.Punch[k].Time;
    }
  }
  if (c.FinishPunch.Code != -1) {
    int cy = gdi.getCY();
    gdi.addString("", cy, xp, 0, "M�l");
    gdi.addStringUT(cy, xp3, 0, formatTimeHMS(c.FinishPunch.Time % (24*3600)));

    if (start != NOTIME) {
      int legTime = analyzePunch(c.FinishPunch, start, accTime, days);
      if (legTime > 0)
        gdi.addStringUT(cy, xp5-gdi.scaleLength(10), textRight, formatTime(legTime));

      gdi.addStringUT(cy, xp5 + gdi.scaleLength(40), textRight, formatTime(days*3600*24 + accTime));
    }
    gdi.addString("", 1, "Time: X#" + formatTime(days*3600*24 + accTime));
  }

  if (forPrinter) {
    gdi.dropLine(1);

    vector< pair<string, int> > lines;
    oe->getExtraLines("SPExtra", lines);

    for (size_t k = 0; k < lines.size(); k++) {
      gdi.addStringUT(lines[k].second, lines[k].first);
    }
    if (lines.size()>0)
      gdi.dropLine(0.5);

    gdi.addString("", fontSmall, "Av MeOS: www.melin.nu/meos");
  }
}

int TabSI::analyzePunch(SIPunch &p, int &start, int &accTime, int &days) {
  int newAccTime = p.Time - start;
  if (newAccTime < 0) {
    newAccTime += 3600 * 24;
    if (accTime > 12 * 3600)
      days++;
  }
  else if (newAccTime < accTime - 12 * 3600) {
    days++;
  }
  int legTime = newAccTime - accTime;
  accTime = newAccTime;
  return legTime;
}

void TabSI::generateSplits(int cardId, gdioutput &gdi) {
  gdioutput gdiprint(2.0, gdi.getEncoding(), gdi.getHWND(), splitPrinter);
  printCard(gdiprint, cardId, true);
  gdiprint.print(splitPrinter, oe, false, true);
}

void TabSI::createCompetitionFromCards(gdioutput &gdi) {
  oe->newCompetition(lang.tl("Ny t�vling"));
  gdi.setWindowTitle("");
  map<size_t, int> hashCount;
  vector< pair<size_t, SICard *> > cards;
  int zeroTime = 3600 * 24;
  for (list<pair<int, SICard> >::iterator it = savedCards.begin(); it != savedCards.end(); ++it) {
    size_t hash = 0;
    if (it->second.StartPunch.Code != -1 && it->second.StartPunch.Time > 0)
      zeroTime = min<int>(zeroTime, it->second.StartPunch.Time);

    for (unsigned k = 0; k < it->second.nPunch; k++) {
      hash = 997 * hash + (it->second.Punch[k].Code-30);
      if (it->second.Punch[k].Code != -1 && it->second.Punch[k].Time > 0)
        zeroTime = min<int>(zeroTime, it->second.Punch[k].Time);
    }
    pair<int, SICard *> p(hash, &it->second);
    ++hashCount[hash];
    cards.push_back(p);
  }

  zeroTime -= 3600;
  if (zeroTime < 0)
    zeroTime += 3600 * 24;
  zeroTime -= zeroTime % 1800;
  oe->setZeroTime(formatTime(zeroTime));

  int course = 0;
  for (size_t k = 0; k < cards.size(); k++) {
    if (!hashCount.count(cards[k].first))
      continue;
    int count = hashCount[cards[k].first];
    if (count < 5 && count < int(cards.size()) /2)
      continue;

    pCourse pc = oe->addCourse(lang.tl("Bana ") + itos(++course));
    for (unsigned j = 0; j < cards[k].second->nPunch; j++) {
      pc->addControl(cards[k].second->Punch[j].Code);
    }
    oe->addClass(lang.tl("Klass ") + itos(course), pc->getId());
    hashCount.erase(cards[k].first);
  }

  // Add remaining classes if suitable
  for (size_t k = 0; k < cards.size(); k++) {
    if (!hashCount.count(cards[k].first))
      continue;
    int count = hashCount[cards[k].first];
    if (count == 1)
      continue; // Don't allow singelton runner classes

    vector<pClass> cls;
    int dist = oe->findBestClass(*cards[k].second, cls);

    if (abs(dist) > 3) {
      pCourse pc = oe->addCourse(lang.tl("Bana ") + itos(++course));
      for (unsigned j = 0; j < cards[k].second->nPunch; k++) {
        pc->addControl(cards[k].second->Punch[j].Code);
      }
      oe->addClass(lang.tl("Klass ") + itos(course), pc->getId());
      hashCount.erase(cards[k].first);
    }
  }

  // Add competitors
  for (size_t k = 0; k < cards.size(); k++) {
    if (oe->isCardRead(*cards[k].second))
      continue;

    vector<pClass> cls;
    oe->findBestClass(*cards[k].second, cls);

    if (!cls.empty()) {
      string name;
      if (cards[k].second->FirstName[0])
        name = string(cards[k].second->FirstName);
      if (cards[k].second->LastName[0])
        name += " " + string(cards[k].second->LastName);

      if (name.empty())
        name = lang.tl("Bricka X#" + itos(cards[k].second->CardNumber));

      oe->addRunner(name, string(cards[k].second->Club), cls[0]->getId(),
                          cards[k].second->CardNumber, 0, true);

      processInsertCard(*cards[k].second);
    }
  }

  TabList &tc = dynamic_cast<TabList &>(*gdi.getTabs().get(TListTab));
  tc.loadPage(gdi, "ResultIndividual");
}

void TabSI::StoredStartInfo::checkAge() {
  DWORD t = GetTickCount();
  const int minuteLimit = 3;
  if (t > age && (t - age) > (1000*60*minuteLimit)) {
    clear();
  }
  age = t;
}

void TabSI::StoredStartInfo::clear() {
  age = GetTickCount();
  storedName.clear();
  storedCardNo.clear();
  storedClub.clear();
  storedFee.clear();
  storedPhone.clear();
  rentState = false;
  storedStartTime.clear();
  hasPaid = false;
  payMode = 1000;
  //allStages = lastAllStages; // Always use last setting
  storedClassId = 0;
}

void TabSI::clearCompetitionData() {
  savedCardUniqueId = 1;
  checkedCardFlags.clear();
  currentAssignIndex = 0;
}

SICard &TabSI::getCard(int id) const {
  if (id < int(savedCards.size() / 2)) {
    for (list< pair<int, SICard> >::const_iterator it = savedCards.begin(); it != savedCards.end(); ++it){
      if (it->first==id)
        return const_cast<SICard &>(it->second);
    }
  }
  else {
    for (list< pair<int, SICard> >::const_reverse_iterator it = savedCards.rbegin(); it != savedCards.rend(); ++it){
      if (it->first==id)
        return const_cast<SICard &>(it->second);
    }
  }
  throw meosException("Interal error");
}

bool compareCardNo(const pRunner &r1, const pRunner &r2) {
  int c1 = r1->getCardNo();
  int c2 = r2->getCardNo();
  if (c1 != c2)
    return c1 < c2;
  int f1 = r1->getFinishTime();
  int f2 = r2->getFinishTime();
  if (f1 != f2)
    return f1 < f2;

  return false;
}

string TabSI::getCardInfo(bool param, vector<int> &count) const {
  if (!param) {
    assert(count.size() == 8);
    return "Totalt antal unika avbockade brickor: X#" + itos(count[CNFCheckedAndUsed] + 
                                                             count[CNFChecked] + 
                                                             count[CNFCheckedNotRented] + 
                                                             count[CNFCheckedRentAndNotRent]);
  }
  count.clear();
  count.resize(8);
  for (map<int, CardNumberFlags>::const_iterator it = checkedCardFlags.begin(); 
    it != checkedCardFlags.end(); ++it) {
      ++count[it->second];
  }

  string msg = "Uthyrda: X, Egna: Y, Avbockade uthyrda: Z#" + itos(count[CNFUsed] + count[CNFCheckedAndUsed]) + 
                                                        "#" + itos(count[CNFNotRented] + count[CNFCheckedNotRented]) + 
                                                        "#" + itos(count[CNFCheckedAndUsed]);

  return msg;
}

void TabSI::showCheckCardStatus(gdioutput &gdi, const string &cmd) {
  vector<pRunner> r;
  const int cx = gdi.getCX();
  const int col1 = gdi.scaleLength(50);
  const int col2 = gdi.scaleLength(200);
 
  if (cmd == "init") {
    gdi.disableInput("Interactive");
    gdi.disableInput("Database");
    gdi.disableInput("PrintSplits");
    gdi.disableInput("UseManualInput");
    gdi.fillDown();   
    gdi.addString("", 10, "help:checkcards");

    gdi.dropLine();
    gdi.fillRight();
    gdi.pushX();
    gdi.addButton("CCSReport", "Rapport", SportIdentCB);
    gdi.addButton("CCSClear", "Nollst�ll", SportIdentCB, 
                  "Nollst�ll minnet; markera alla brickor som icke avbockade");
    gdi.addButton("CCSPrint", "Skriv ut...", SportIdentCB);

    gdi.popX();
    gdi.dropLine(3);
    gdi.fillDown();
    gdi.setRestorePoint("CCSInit");
    showCheckCardStatus(gdi, "fillrunner");
    showCheckCardStatus(gdi, "stat");
    showCheckCardStatus(gdi, "tickoff");
    return;
  }
  else if (cmd == "fillrunner") {
    oe->getRunners(0, 0, r);

    for (size_t k = 0; k < r.size(); k++) {
      int cno = r[k]->getCardNo();
      if (cno == 0)
        continue;
      int cf = checkedCardFlags[cno];
      if (r[k]->getDI().getInt("CardFee") != 0)
        checkedCardFlags[cno] = CardNumberFlags(cf | CNFUsed);
      else
        checkedCardFlags[cno] = CardNumberFlags(cf | CNFNotRented);
    }
  }
  else if (cmd == "stat") {
    vector<int> count;
    gdi.addString("CardInfo", fontMediumPlus, getCardInfo(true, count));
    gdi.addString("CardTicks", 0, getCardInfo(false, count));
    if (count[CNFCheckedRentAndNotRent] + count[CNFRentAndNotRent] > 0) {
      oe->getRunners(0, 0, r);
      stable_sort(r.begin(), r.end(), compareCardNo);
      gdi.dropLine();
      string msg = "Brickor markerade som b�de uthyrda och egna: X#" + itos(count[CNFCheckedRentAndNotRent] + count[CNFRentAndNotRent]);
      gdi.addString("", 1, msg).setColor(colorDarkRed);
      gdi.dropLine(0.5);
      for (size_t k = 0; k < r.size(); k++) {
        int cno = r[k]->getCardNo();
        if (cno == 0 || r[k]->getRaceNo() > 0)
            continue;

        if (checkedCardFlags[cno] == CNFCheckedRentAndNotRent ||
            checkedCardFlags[cno] == CNFRentAndNotRent) {
          int yp = gdi.getCY();
          string cp = r[k]->getCompleteIdentification();
          bool rent = r[k]->getDI().getInt("CardFee") != 0;
          string info = rent ? (" (" + lang.tl("Hyrd") + ")") : "";
          gdi.addStringUT(yp, cx, 0, itos(cno) + info);
          gdi.addStringUT(yp, cx + col2, 0, cp);
        }
      }
    }
  }
  else if (cmd == "report") {
    oe->getRunners(0, 0, r);
    stable_sort(r.begin(), r.end(), compareCardNo);
    bool showHead = false;
    int count = 0;
    for (size_t k = 0; k < r.size(); k++) {
      int cno = r[k]->getCardNo();
      if (cno == 0)
        continue;
      if (r[k]->getRaceNo() > 0)
        continue;
      CardNumberFlags f = checkedCardFlags[cno];
      if (f == CNFRentAndNotRent || f == CNFUsed) {
        if (!showHead) {
          gdi.dropLine();
          string msg = "Uthyrda brickor som inte avbockats";
          gdi.addString("", fontMediumPlus, msg);
          gdi.fillDown();
          gdi.dropLine(0.5);
          showHead = true;
        } 
        int yp = gdi.getCY();
        gdi.addStringUT(yp, cx, 0, itos(++count));
        gdi.addStringUT(yp, cx + col1, 0, itos(cno));
        string cp = r[k]->getCompleteIdentification();

        if (r[k]->getStatus() != StatusUnknown)
          cp += " " + r[k]->getStatusS();
        else
          cp += MakeDash(" -");

        int s = r[k]->getStartTime();
        int f = r[k]->getFinishTime();
        if (s> 0 || f>0) {
          cp += ", " + (s>0 ? r[k]->getStartTimeS() : string("?")) + MakeDash(" - ") 
                 + (f>0 ? r[k]->getFinishTimeS() : string("?"));  
        }
        gdi.addStringUT(yp, cx + col2, 0, cp);
      }
    }

    if (!showHead) {
      gdi.dropLine();
      string msg = "Alla uthyrda brickor har bockats av.";
      gdi.addString("", fontMediumPlus, msg).setColor(colorGreen);
    }
  }
  else if (cmd == "tickoff") {
    SICard sic;
    sic.clear(0);
    for (map<int, CardNumberFlags>::const_iterator it = checkedCardFlags.begin(); 
        it != checkedCardFlags.end(); ++it) {
      int stat = it->second;
      if (stat & CNFChecked) {
        sic.CardNumber = it->first;
        checkCard(gdi, sic, false);
      }
    }
    gdi.refresh();
    return;
  }
  checkHeader = false;
  gdi.dropLine();
}

void TabSI::checkCard(gdioutput &gdi, const SICard &card, bool updateAll) {
  bool wasChecked = (checkedCardFlags[card.CardNumber] & CNFChecked) != 0 && updateAll;

  checkedCardFlags[card.CardNumber] = CardNumberFlags(checkedCardFlags[card.CardNumber] | CNFChecked);
  vector<int> count;
  if (!checkHeader) {
    checkHeader = true;
    gdi.addString("", fontMediumPlus, "Avbockade brickor:");
    gdi.dropLine(0.5);
    cardPosX = gdi.getCX();
    cardPosY = gdi.getCY();
    cardOffsetX = gdi.scaleLength(60);
    cardNumCol = 12;
    cardCurrentCol = 0;
  }

  if (updateAll) {
    gdi.setTextTranslate("CardInfo", getCardInfo(true, count));
    gdi.setTextTranslate("CardTicks", getCardInfo(false, count));
  }
  TextInfo &ti = gdi.addStringUT(cardPosY, cardPosX + cardCurrentCol * cardOffsetX, 0, itos(card.CardNumber));
  if (wasChecked)
    ti.setColor(colorRed);
  if (++cardCurrentCol >= cardNumCol) {
    cardCurrentCol = 0;
    cardPosY += gdi.getLineHeight();
  }

  if (updateAll) {
    gdi.scrollToBottom();
    gdi.refreshFast();
  }
}

void TabSI::generatePayModeWidget(gdioutput &gdi) const {
  vector< pair<string, size_t> > pm;
  oe->getPayModes(pm);
  assert(pm.size() > 0);
  if (pm.size() == 1) {
    assert(pm[0].second == 0);
    gdi.addCheckbox("Paid", "#" + pm[0].first, SportIdentCB, storedInfo.hasPaid);
  }
  else {
    pm.insert(pm.begin(), make_pair(lang.tl("Faktureras"), 1000));
    gdi.addSelection("PayMode", 110, 100, SportIdentCB);
    gdi.addItem("PayMode", pm);
    gdi.selectItemByData("PayMode", storedInfo.payMode);
    gdi.autoGrow("PayMode");
  }
}

bool TabSI::writePayMode(gdioutput &gdi, int amount, oRunner &r) {
  int paid = 0;
  bool hasPaid = false;
      
  if (gdi.hasField("PayMode"))
    hasPaid = gdi.getSelectedItem("PayMode").first != 1000;

  bool fixPay = gdi.isChecked("Paid");
  if (hasPaid || fixPay) {
    paid = amount;
  }

  r.getDI().setInt("Paid", paid);
  if (hasPaid) {
    r.setPaymentMode(gdi.getSelectedItem("PayMode").first);
  }
  return hasPaid || fixPay;
}
