///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// THaVDCPlane                                                               //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Here:                                                                     //
//        Units of measurements:                                             //
//        For cluster position (center) and size  -  wires;                  //
//        For X, Y, and Z coordinates of track    -  meters;                 //
//        For Theta and Phi angles of track       -  in tangents.            //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

//#define CLUST_RAWDATA_HACK

#include "THaVDC.h"
#include "THaVDCPlane.h"
#include "THaVDCWire.h"
#include "THaVDCChamber.h"
#include "THaVDCCluster.h"
#include "THaVDCHit.h"
#include "THaDetMap.h"
#include "THaVDCAnalyticTTDConv.h"
#include "THaEvData.h"
#include "TString.h"
#include "TClass.h"
#include "TMath.h"
#include "VarDef.h"
#include "THaApparatus.h"
#include "THaTriggerTime.h"

#include <cstring>
#include <vector>
#include <iostream>
#include <cassert>

#ifdef CLUST_RAWDATA_HACK
#include <fstream>
#endif

using namespace std;


//_____________________________________________________________________________
THaVDCPlane::THaVDCPlane( const char* name, const char* description,
			  THaDetectorBase* parent )
  : THaSubDetector(name,description,parent), /*fTable(0),*/ fTTDConv(0),
    fVDC(0), fglTrg(0)
{
  // Constructor

  // Since TCloneArrays can resize, the size here is fairly unimportant
  fHits     = new TClonesArray("THaVDCHit", 20 );
  fClusters = new TClonesArray("THaVDCCluster", 5 );
  fWires    = new TClonesArray("THaVDCWire", 368 );

  fNpass = 0;
  fMinTdiff = 0.0;
  fMaxTdiff = 2e-7;  // 200ns correspond to ~10mm, will accept all

  fVDC = dynamic_cast<THaVDC*>( GetMainDetector() );
}

//_____________________________________________________________________________
void THaVDCPlane::MakePrefix()
{
  // Special treatment of the prefix for VDC planes: We don't want variable
  // names such as "R.vdc.uv1.u.x", but rather "R.vdc.u1.x".

  TString basename;
  THaDetectorBase* uv_plane = GetParent();

  if( fVDC )
    basename = fVDC->GetPrefix();
  if( fName.Contains("u") )
    basename.Append("u");
  else if ( fName.Contains("v") )
    basename.Append("v");
  if( uv_plane && strstr( uv_plane->GetName(), "uv1" ))
    basename.Append("1.");
  else if( uv_plane && strstr( uv_plane->GetName(), "uv2" ))
    basename.Append("2.");
  if( basename.Length() == 0 )
    basename = fName + ".";

  delete [] fPrefix;
  fPrefix = new char[ basename.Length()+1 ];
  strcpy( fPrefix, basename.Data() );
}

//_____________________________________________________________________________
Int_t THaVDCPlane::ReadDatabase( const TDatime& date )
{
  // Allocate TClonesArray objects and load plane parameters from database

  FILE* file = OpenFile( date );
  if( !file ) return kFileError;

  // Use default values until ready to read from database

  static const char* const here = "ReadDatabase";
  const int LEN = 100;
  char buff[LEN];

  // Build the search tag and find it in the file. Search tags
  // are of form [ <prefix> ], e.g. [ R.vdc.u1 ].
  TString tag(fPrefix); tag.Chop(); // delete trailing dot of prefix
  tag.Prepend("["); tag.Append("]");
  TString line;
  bool found = false;
  while (!found && fgets (buff, LEN, file) != NULL) {
    char* buf = ::Compress(buff);  //strip blanks
    line = buf;
    delete [] buf;
    if( line.EndsWith("\n") ) line.Chop();  //delete trailing newline
    if ( tag == line )
      found = true;
  }
  if( !found ) {
    Error(Here(here), "Database section \"%s\" not found! "
	  "Initialization failed", tag.Data() );
    fclose(file);
    return kInitError;
  }

  //Found the entry for this plane, so read data
  Int_t nWires = 0;    // Number of wires to create
  Int_t prev_first = 0, prev_nwires = 0;
  // Set up the detector map
  fDetMap->Clear();
  do {
    fgets( buff, LEN, file );
    // bad kludge to allow a variable number of detector map lines
    if( strchr(buff, '.') ) // any floating point number indicates end of map
      break;
    // Get crate, slot, low channel and high channel from file
    Int_t crate, slot, lo, hi;
    if( sscanf( buff, "%6d %6d %6d %6d", &crate, &slot, &lo, &hi ) != 4 ) {
      if( *buff ) buff[strlen(buff)-1] = 0; //delete trailing newline
      Error( Here(here), "Error reading detector map line: %s", buff );
      fclose(file);
      return kInitError;
    }
    Int_t first = prev_first + prev_nwires;
    // Add module to the detector map
    fDetMap->AddModule(crate, slot, lo, hi, first);
    prev_first = first;
    prev_nwires = (hi - lo + 1);
    nWires += prev_nwires;
  } while( *buff );  // sanity escape
  // Load z, wire beginning postion, wire spacing, and wire angle
  sscanf( buff, "%15lf %15lf %15lf %15lf", &fZ, &fWBeg, &fWSpac, &fWAngle );
  fWAngle *= TMath::Pi()/180.0; // Convert to radians
  fSinAngle = TMath::Sin( fWAngle );
  fCosAngle = TMath::Cos( fWAngle );

  // Load drift velocity (will be used to initialize crude Time to Distance
  // converter)
  fscanf(file, "%15lf", &fDriftVel);
  fgets(buff, LEN, file); // Read to end of line
  fgets(buff, LEN, file); // Skip line

  // first read in the time offsets for the wires
  float* wire_offsets = new float[nWires];
  int*   wire_nums    = new int[nWires];

  for (int i = 0; i < nWires; i++) {
    int wnum = 0;
    float offset = 0.0;
    fscanf(file, " %6d %15f", &wnum, &offset);
    wire_nums[i] = wnum-1; // Wire numbers in file start at 1
    wire_offsets[i] = offset;
  }


  // now read in the time-to-drift-distance lookup table
  // data (if it exists)
//   fgets(buff, LEN, file); // read to the end of line
//   fgets(buff, LEN, file);
//   if(strncmp(buff, "TTD Lookup Table", 16) == 0) {
//     // if it exists, read the data in
//     fscanf(file, "%le", &fT0);
//     fscanf(file, "%d", &fNumBins);

//     // this object is responsible for the memory management
//     // of the lookup table
//     delete [] fTable;
//     fTable = new Float_t[fNumBins];
//     for(int i=0; i<fNumBins; i++) {
//       fscanf(file, "%e", &(fTable[i]));
//     }
//   } else {
//     // if not, set some reasonable defaults and rewind the file
//     fT0 = 0.0;
//     fNumBins = 0;
//     fTable = NULL;
//     cout<<"Could not find lookup table header: "<<buff<<endl;
//     fseek(file, -strlen(buff), SEEK_CUR);
//   }

  // Define time-to-drift-distance converter
  // Currently, we use the analytic converter.
  // FIXME: Let user choose this via a parameter
  delete fTTDConv;
  fTTDConv = new THaVDCAnalyticTTDConv(fDriftVel);

  //THaVDCLookupTTDConv* ttdConv = new THaVDCLookupTTDConv(fT0, fNumBins, fTable);

  // Now initialize wires (those wires... too lazy to initialize themselves!)
  // Caution: This may not correspond at all to actual wire channels!
  for (int i = 0; i < nWires; i++) {
    THaVDCWire* wire = new((*fWires)[i])
      THaVDCWire( i, fWBeg+i*fWSpac, wire_offsets[i], fTTDConv );
    if( wire_nums[i] < 0 )
      wire->SetFlag(1);
  }
  delete [] wire_offsets;
  delete [] wire_nums;

  // Set location of chamber center (in detector coordinates)
  fOrigin.SetXYZ( 0.0, 0.0, fZ );

  // Read additional parameters (not in original database)
  // For backward compatibility with database version 1, these parameters
  // are in an optional section, labelled [ <prefix>.extra_param ]
  // (e.g. [ R.vdc.u1.extra_param ]) or [ R.extra_param ].  If this tag is
  // not found, a warning is printed and defaults are used.

  tag = "["; tag.Append(fPrefix); tag.Append("extra_param]");
  TString tag2(tag);
  found = false;
  rewind(file);
  while (!found && fgets(buff, LEN, file) != NULL) {
    char* buf = ::Compress(buff);  //strip blanks
    line = buf;
    delete [] buf;
    if( line.EndsWith("\n") ) line.Chop();  //delete trailing newline
    if ( tag == line )
      found = true;
  }
  if( !found ) {
    // Poor man's default key search
    rewind(file);
    tag2 = fPrefix;
    Ssiz_t pos = tag2.Index(".");
    if( pos != kNPOS )
      tag2 = tag2(0,pos+1);
    else
      tag2.Append(".");
    tag2.Prepend("[");
    tag2.Append("extra_param]");
    while (!found && fgets(buff, LEN, file) != NULL) {
      char* buf = ::Compress(buff);  //strip blanks
      line = buf;
      delete [] buf;
      if( line.EndsWith("\n") ) line.Chop();  //delete trailing newline
      if ( tag2 == line )
	found = true;
    }
  }
  if( found ) {
    if( fscanf(file, "%lf %lf", &fTDCRes, &fT0Resolution) != 2) {
      Error( Here(here), "Error reading TDC scale and T0 resolution\n"
	     "Line = %s\nFix database.", buff );
      fclose(file);
      return kInitError;
    }
    fgets(buff, LEN, file); // Read to end of line
    if( fscanf( file, "%d %d %d %d %d %lf %lf", &fMinClustSize, &fMaxClustSpan,
		&fNMaxGap, &fMinTime, &fMaxTime, &fMinTdiff, &fMaxTdiff ) != 7 ) {
      Error( Here(here), "Error reading min_clust_size, max_clust_span, "
	     "max_gap, min_time, max_time, min_tdiff, max_tdiff.\n"
	     "Line = %s\nFix database.", buff );
      fclose(file);
      return kInitError;
    }
    fgets(buff, LEN, file); // Read to end of line
    // Time-to-distance converter parameters
    if( THaVDCAnalyticTTDConv* analytic_conv =
	dynamic_cast<THaVDCAnalyticTTDConv*>(fTTDConv) ) {
      // THaVDCAnalyticTTDConv
      // Format: 4*A1 4*A2 dtime(s)  (9 floats)
      Double_t A1[4], A2[4], dtime;
      if( fscanf(file, "%lf %lf %lf %lf %lf %lf %lf %lf %lf",
		 &A1[0], &A1[1], &A1[2], &A1[3],
		 &A2[0], &A2[1], &A2[2], &A2[3], &dtime ) != 9) {
	Error( Here(here), "Error reading THaVDCAnalyticTTDConv parameters\n"
	       "Line = %s\nExpect 9 floating point numbers. Fix database.",
	       buff );
	fclose(file);
	return kInitError;
      } else {
	analytic_conv->SetParameters( A1, A2, dtime );
      }
    }
//     else if( (... *conv = dynamic_cast<...*>(fTTDConv)) ) {
//       // handle parameters of other TTD converters here
//     }

    fgets(buff, LEN, file); // Read to end of line

    Double_t h, w;

    if( fscanf(file, "%lf %lf", &h, &w) != 2) {
	Error( Here(here), "Error reading height/width parameters\n"
	       "Line = %s\nExpect 2 floating point numbers. Fix database.",
	       buff );
	fclose(file);
	return kInitError;
      } else {
	   fSize[0] = h/2.0;
	   fSize[1] = w/2.0;
      }

    fgets(buff, LEN, file); // Read to end of line
    // Sanity checks
    if( fMinClustSize < 1 || fMinClustSize > 6 ) {
      Error( Here(here), "Invalid min_clust_size = %d, must be betwwen 1 and "
	     "6. Fix database.", fMinClustSize );
      fclose(file);
      return kInitError;
    }
    if( fMaxClustSpan < 2 || fMaxClustSpan > 12 ) {
      Error( Here(here), "Invalid max_clust_span = %d, must be betwwen 1 and "
	     "12. Fix database.", fMaxClustSpan );
      fclose(file);
      return kInitError;
    }
    if( fNMaxGap < 0 || fNMaxGap > 2 ) {
      Error( Here(here), "Invalid max_gap = %d, must be betwwen 0 and 2. "
	     "Fix database.", fNMaxGap );
      fclose(file);
      return kInitError;
    }
    if( fMinTime < 0 || fMinTime > 4095 ) {
      Error( Here(here), "Invalid min_time = %d, must be betwwen 0 and 4095. "
	     "Fix database.", fMinTime );
      fclose(file);
      return kInitError;
    }
    if( fMaxTime < 1 || fMaxTime > 4096 || fMinTime >= fMaxTime ) {
      Error( Here(here), "Invalid max_time = %d. Must be between 1 and 4096 "
	     "and >= min_time = %d. Fix database.", fMaxTime, fMinTime );
      fclose(file);
      return kInitError;
    }
  } else {
    Warning( Here(here), "No database section \"%s\" or \"%s\" found. "
	     "Using defaults.", tag.Data(), tag2.Data() );
    fTDCRes = 5.0e-10;  // 0.5 ns/chan = 5e-10 s /chan
    fT0Resolution = 6e-8; // 60 ns --- crude guess
    fMinClustSize = 4;
    fMaxClustSpan = 7;
    fNMaxGap = 0;
    fMinTime = 800;
    fMaxTime = 2200;
    fMinTdiff = 3e-8;   // 30ns  -> ~20 deg track angle
    fMaxTdiff = 1.5e-7; // 150ns -> ~60 deg track angle

    if( THaVDCAnalyticTTDConv* analytic_conv =
	dynamic_cast<THaVDCAnalyticTTDConv*>(fTTDConv) ) {
      Double_t A1[4], A2[4], dtime = 4e-9;
      A1[0] = 2.12e-3;
      A1[1] = A1[2] = A1[3] = 0.0;
      A2[0] = -4.2e-4;
      A2[1] = 1.3e-3;
      A2[2] = 1.06e-4;
      A2[3] = 0.0;
      analytic_conv->SetParameters( A1, A2, dtime );
    }
  }

  THaDetectorBase *sdet = GetParent();
  if( sdet )
    fOrigin += sdet->GetOrigin();

  // finally, find the timing-offset to apply on an event-by-event basis
  //FIXME: time offset handling should go into the enclosing apparatus -
  //since not doing so leads to exactly this kind of mess:
  THaApparatus* app = GetApparatus();
  const char* nm = "trg"; // inside an apparatus, the apparatus name is assumed
  if( !app ||
      !(fglTrg = dynamic_cast<THaTriggerTime*>(app->GetDetector(nm))) ) {
    Warning(Here(here),"Trigger-time detector \"%s\" not found. "
	    "Event-by-event time offsets will NOT be used!!",nm);
  }

  fIsInit = true;
  fclose(file);

  return kOK;
}

//_____________________________________________________________________________
Int_t THaVDCPlane::DefineVariables( EMode mode )
{
  // initialize global variables

  if( mode == kDefine && fIsSetup ) return kOK;
  fIsSetup = ( mode == kDefine );

  // Register variables in global list

  RVarDef vars[] = {
    { "nhit",   "Number of hits",             "GetNHits()" },
    { "wire",   "Active wire numbers",        "fHits.THaVDCHit.GetWireNum()" },
    { "rawtime","Raw TDC values of wires",    "fHits.THaVDCHit.fRawTime" },
    { "time",   "TDC values of active wires", "fHits.THaVDCHit.fTime" },
    { "dist",   "Drift distances",            "fHits.THaVDCHit.fDist" },
    { "ddist",  "Drft dist uncertainty",      "fHits.THaVDCHit.fdDist" },
    { "trdist", "Dist. from track",           "fHits.THaVDCHit.ftrDist" },
    { "ltrdist","Dist. from local track",     "fHits.THaVDCHit.fltrDist" },
    { "trknum", "Track number (0=unused)",    "fHits.THaVDCHit.fTrkNum" },
    { "clsnum", "Cluster number (-1=unused)",    "fHits.THaVDCHit.fClsNum" },
    { "nclust", "Number of clusters",         "GetNClusters()" },
    { "clsiz",  "Cluster sizes",              "fClusters.THaVDCCluster.GetSize()" },
    { "clpivot","Cluster pivot wire num",     "fClusters.THaVDCCluster.GetPivotWireNum()" },
    { "clpos",  "Cluster intercepts (m)",     "fClusters.THaVDCCluster.fInt" },
    { "slope",  "Cluster best slope",         "fClusters.THaVDCCluster.fSlope" },
    { "lslope", "Cluster local (fitted) slope","fClusters.THaVDCCluster.fLocalSlope" },
    { "t0",     "Timing offset (s)",          "fClusters.THaVDCCluster.fT0" },
    { "sigsl",  "Cluster slope error",        "fClusters.THaVDCCluster.fSigmaSlope" },
    { "sigpos", "Cluster position error (m)", "fClusters.THaVDCCluster.fSigmaInt" },
    { "sigt0",  "Timing offset error (s)",    "fClusters.THaVDCCluster.fSigmaT0" },
    { "clchi2", "Cluster chi2",               "fClusters.THaVDCCluster.fChi2" },
    { "clndof", "Cluster NDoF",               "fClusters.THaVDCCluster.fNDoF" },
    { "cltcor", "Cluster Time correction",    "fClusters.THaVDCCluster.fTimeCorrection" },
    { "cltridx","Idx of track assoc w/cluster", "fClusters.THaVDCCluster.GetTrackIndex()" },
    { "cltrknum", "Cluster track number (0=unused)", "fClusters.THaVDCCluster.fTrkNum" },
    { "clbeg", "Cluster start wire",          "fClusters.THaVDCCluster.fClsBeg" },
    { "clend", "Cluster end wire",            "fClusters.THaVDCCluster.fClsEnd" },
    { "npass", "Number of hit passes for cluster", "GetNpass()" },
    { 0 }
  };
  return DefineVarsFromList( vars, mode );

}

//_____________________________________________________________________________
THaVDCPlane::~THaVDCPlane()
{
  // Destructor.

  if( fIsSetup )
    RemoveVariables();
  delete fWires;
  delete fHits;
  delete fClusters;
  delete fTTDConv;
//   delete [] fTable;
}

//_____________________________________________________________________________
void THaVDCPlane::Clear( Option_t* )
{
  // Clears the contents of the and hits and clusters
  fNHits = fNWiresHit = 0;
  fHits->Clear();
  fClusters->Clear();
}

//_____________________________________________________________________________
Int_t THaVDCPlane::Decode( const THaEvData& evData )
{
  // Converts the raw data into hit information
  // Logical wire numbers a defined by the detector map. Wire number 0
  // corresponds to the first defined channel, etc.

  // TODO: Support "reversed" detector map modules a la MWDC

  if (!evData.IsPhysicsTrigger()) return -1;

  // the event's T0-shift, due to the trigger-type
  // only an issue when adding in un-retimed trigger types
  Double_t evtT0=0;
  if ( fglTrg && fglTrg->Decode(evData)==kOK ) evtT0 = fglTrg->TimeOffset();

  Int_t nextHit = 0;

  bool only_fastest_hit = false, no_negative = false;
  if( fVDC ) {
    // If true, add only the first (earliest) hit for each wire
    only_fastest_hit = fVDC->TestBit(THaVDC::kOnlyFastest);
    // If true, ignore negative drift times completely
    no_negative      = fVDC->TestBit(THaVDC::kIgnoreNegDrift);
  }

  // Loop over all detector modules for this wire plane
  for (Int_t i = 0; i < fDetMap->GetSize(); i++) {
    THaDetMap::Module * d = fDetMap->GetModule(i);

    // Get number of channels with hits
    Int_t nChan = evData.GetNumChan(d->crate, d->slot);
    for (Int_t chNdx = 0; chNdx < nChan; chNdx++) {
      // Use channel index to loop through channels that have hits

      Int_t chan = evData.GetNextChan(d->crate, d->slot, chNdx);
      if (chan < d->lo || chan > d->hi)
	continue; //Not part of this detector

      // Wire numbers and channels go in the same order ...
      Int_t wireNum  = d->first + chan - d->lo;
      THaVDCWire* wire = GetWire(wireNum);
      if( !wire || wire->GetFlag() != 0 ) continue;

      // Get number of hits for this channel and loop through hits
      Int_t nHits = evData.GetNumHits(d->crate, d->slot, chan);

      Int_t max_data = -1;
      Double_t toff = wire->GetTOffset();

      for (Int_t hit = 0; hit < nHits; hit++) {

	// Now get the TDC data for this hit
	Int_t data = evData.GetData(d->crate, d->slot, chan, hit);

	// Convert the TDC value to the drift time.
	// Being perfectionist, we apply a 1/2 channel correction to the raw
	// TDC data to compensate for the fact that the TDC truncates, not
	// rounds, the data.
	Double_t xdata = static_cast<Double_t>(data) + 0.5;
	Double_t time = fTDCRes * (toff - xdata) - evtT0;

	// If requested, ignore hits with negative drift times
	// (due to noise or miscalibration). Use with care.
	// If only fastest hit requested, find maximum TDC value and record the
	// hit after the hit loop is done (see below).
	// Otherwise just record all hits.
	if( !no_negative || time > 0.0 ) {
	  if( only_fastest_hit ) {
	    if( data > max_data )
	      max_data = data;
	  } else
	    new( (*fHits)[nextHit++] )  THaVDCHit( wire, data, time );
	}

    // Count all hits and wires with hits
    //    fNWiresHit++;

      } // End hit loop

      // If we are only interested in the hit with the largest TDC value
      // (shortest drift time), it is recorded here.
      if( only_fastest_hit && max_data>0 ) {
	Double_t xdata = static_cast<Double_t>(max_data) + 0.5;
	Double_t time = fTDCRes * (toff - xdata) - evtT0;
	new( (*fHits)[nextHit++] ) THaVDCHit( wire, max_data, time );
      }
    } // End channel index loop
  } // End slot loop

  // Sort the hits in order of increasing wire number and (for the same wire
  // number) increasing time (NOT rawtime)

  fHits->Sort();

  if ( fDebug > 3 ) {
    printf("\nVDC %s:\n",GetPrefix());
    int ncol=4;
    for (int i=0; i<ncol; i++) {
      printf("     Wire    TDC  ");
    }
    printf("\n");

    for (int i=0; i<(nextHit+ncol-1)/ncol; i++ ) {
      for (int c=0; c<ncol; c++) {
	int ind = c*nextHit/ncol+i;
	if (ind < nextHit) {
	  THaVDCHit* hit = static_cast<THaVDCHit*>(fHits->At(ind));
	  printf("     %3d    %5d ",hit->GetWireNum(),hit->GetRawTime());
	} else {
	  //	  printf("\n");
	  break;
	}
      }
      printf("\n");
    }
  }

  return 0;

}


//_____________________________________________________________________________
class TimeCut {
public:
  TimeCut( THaVDC* vdc, THaVDCPlane* _plane )
    : hard_cut(false), soft_cut(false), maxdist(0.0), plane(_plane)
  {
    assert(vdc);
    assert(plane);
    if( vdc ) {
      hard_cut = vdc->TestBit(THaVDC::kHardTDCcut);
      soft_cut = vdc->TestBit(THaVDC::kSoftTDCcut);
    }
    if( soft_cut ) {
      maxdist =
	0.5*static_cast<THaVDCChamber*>(plane->GetParent())->GetSpacing();
      if( maxdist == 0.0 )
	soft_cut = false;
    }
  }
  bool operator() ( const THaVDCHit* hit )
  {
    // Only keep hits whose drift times are within sanity cuts
    if( hard_cut ) {
      Double_t rawtime = hit->GetRawTime();
      if( rawtime < plane->GetMinTime() || rawtime > plane->GetMaxTime())
	return false;
    }
    if( soft_cut ) {
      Double_t ratio = hit->GetTime() * plane->GetDriftVel() / maxdist;
      if( ratio < -0.5 || ratio > 1.5 )
	return false;
    }
    return true;
  }
private:
  bool          hard_cut;
  bool          soft_cut;
  double        maxdist;
  THaVDCPlane*  plane;
};

//_____________________________________________________________________________
Int_t THaVDCPlane::FindClusters()
{
  // Reconstruct clusters in a VDC plane
  // Assumes that the wires are numbered such that increasing wire numbers
  // correspond to decreasing physical position.
  // Ignores possibility of overlapping clusters

  TimeCut timecut(fVDC,this);

// #ifndef NDEBUG
//   // bugcheck
//   bool only_fastest_hit = false;
//   if( fVDC )
//     only_fastest_hit = fVDC->TestBit(THaVDC::kOnlyFastest);
// #endif

  Int_t nHits     = GetNHits();   // Number of hits in the plane
  Int_t nUsed = 0;                // Number of wires used in clustering
  Int_t nLastUsed = -1;
  Int_t nextClust = 0;            // Current cluster number
  assert( GetNClusters() == 0 );

  vector <THaVDCHit *> clushits;
  Double_t deltat;
  Bool_t falling;

  fNpass = 0;

  Int_t nwires, span;
  UInt_t j;

  //  Loop while we're making new clusters
  while( nLastUsed != nUsed ){
     fNpass++;
     nLastUsed = nUsed;
     //Loop through all TDC hits
     for( Int_t i = 0; i < nHits; ) {
       clushits.clear();
       falling = kTRUE;

       THaVDCHit* hit = GetHit(i);
       assert(hit);

       if( !timecut(hit) ) {
	       ++i;
	       continue;
       }
       if( hit->GetClsNum() != -1 )
       	  { ++i; continue; }
       // Ensures we don't use this to try and start a new
       // cluster
       hit->SetClsNum(-3);

       // Consider this hit the beginning of a potential new cluster.
       // Find the end of the cluster.
       span = 0;
       nwires = 1;
       while( ++i < nHits ) {
	  THaVDCHit* nextHit = GetHit(i);
	  assert( nextHit );    // should never happen, else bug in Decode
	  if( !timecut(nextHit) )
		  continue;
	  if(    nextHit->GetClsNum() != -1   // -1 is virgin
	      && nextHit->GetClsNum() != -3 ) // -3 was considered to start
		  			      //a clus but is not in cluster
		  continue;
	  Int_t ndif = nextHit->GetWireNum() - hit->GetWireNum();
	  // Do not consider adding hits from a wire that was already
	  // added
	  if( ndif == 0 ) { continue; }
	  assert( ndif >= 0 );
	  // The cluster ends when we encounter a gap in wire numbers.
	  // TODO: cluster should also end if
	  //  DONE (a) it is too big
	  //  DONE (b) drift times decrease again after initial fall/rise (V-shape)
	  //  DONE (c) Enforce reasonable changes in wire-to-wire V-shape

	  // Times are sorted by earliest first when on same wire
	  deltat = nextHit->GetTime() - hit->GetTime();

	  span += ndif;
	  if( ndif > fNMaxGap+1 || span > fMaxClustSpan ){
		  break;
	  }

	  // Make sure the time structure is sensible
	  // If this cluster is rising, wire with falling time
	  // should not be associated in the cluster
	  if( !falling ){
		  if( deltat < fMinTdiff*ndif ||
		      deltat > fMaxTdiff*ndif )
		  { continue; }
	  }

	  if( falling ){
		  // Step is too big, can't be associated
		  if( deltat < -fMaxTdiff*ndif ){ continue; }
		  if( deltat > 0.0 ){
			  // if rise is reasonable and we don't
			  // have a monotonically increasing cluster
			  if( deltat < fMaxTdiff*ndif && span > 1 ){
				  // now we're rising
				  falling = kFALSE;
			  } else {
				  continue;
			  }
		  }
	  }

	  nwires++;
	  if( clushits.size() == 0 ){
		  clushits.push_back(hit);
		  hit->SetClsNum(-2);
		  nUsed++;
	  }
	  clushits.push_back(nextHit);
	  nextHit->SetClsNum(-2);
	  nUsed++;
	  hit = nextHit;
       }
       assert( i <= nHits );
       // Make a new cluster if it is big enough
       // If not, the hits of this i-iteration are ignored
       // Also, make sure that we did indeed see the time
       // spectrum turn around at some point
       if( nwires >= fMinClustSize && !falling ) {
	  THaVDCCluster* clust =
	     new ( (*fClusters)[nextClust++] ) THaVDCCluster(this);

	  for( j = 0; j < clushits.size(); j++ ){
	     clushits[j]->SetClsNum(nextClust-1);
	     clust->AddHit( clushits[j] );
	  }

	  assert( clust->GetSize() > 0 && clust->GetSize() >= nwires );
	  // This is a good cluster candidate. Estimate its position/slope
	  clust->EstTrackParameters();
       } //end new cluster

     } //end loop over hits

  } // end passes over hits

  assert( GetNClusters() == nextClust );

  return nextClust;  // return the number of clusters found
}

//_____________________________________________________________________________
Int_t THaVDCPlane::FitTracks()
{
  // Fit tracks to cluster positions and drift distances.

  Int_t nClust = GetNClusters();
  for (int i = 0; i < nClust; i++) {
    THaVDCCluster* clust = static_cast<THaVDCCluster*>( (*fClusters)[i] );
    if( !clust ) continue;

    // Convert drift times to distances.
    // The conversion algorithm is determined at wire initialization time,
    // i.e. currently in the ReadDatabase() function of this class.
    // The conversion is done with the current value of fSlope in the
    // clusters, i.e. either the rough guess from
    // THaVDCCluster::EstTrackParameters or the global slope from
    // THaVDC::ConstructTracks
    clust->ConvertTimeToDist();

    // Fit drift distances to get intercept, slope.
    clust->FitTrack();

#ifdef CLUST_RAWDATA_HACK
    // HACK: write out cluster info for small-t0 clusters in u1
    if( fName == "u" && !strcmp(GetParent()->GetName(),"uv1") &&
	TMath::Abs(clust->GetT0()) < fT0Resolution/3. &&
	clust->GetSize() <= 6 ) {
      ofstream outp;
      outp.open("u1_cluster_data.out",ios_base::app);
      outp << clust->GetSize() << endl;
      for( int i=clust->GetSize()-1; i>=0; i-- ) {
	outp << clust->GetHit(i)->GetPos() << " "
	     << clust->GetHit(i)->GetDist()
	     << endl;
      }
      outp << 1./clust->GetSlope() << " "
	   << clust->GetIntercept()
	   << endl;
      outp.close();
    }
#endif
  }

  return 0;
}

///////////////////////////////////////////////////////////////////////////////
ClassImp(THaVDCPlane)
