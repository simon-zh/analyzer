#ifndef ROOT_THaVDC
#define ROOT_THaVDC

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// THaVDC                                                                    //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#include "THaTrackingDetector.h"
#include <vector>

class THaVDCUVPlane;
class THaTrack;
class TClonesArray;
class THaVDCUVTrack;

class THaVDC : public THaTrackingDetector {

public:
  THaVDC( const char* name, const char* description = "",
	  THaApparatus* a = NULL );

  virtual ~THaVDC();

  virtual Int_t Decode( const THaEvData& );
  virtual Int_t CoarseTrack( TClonesArray& tracks );
  virtual Int_t FineTrack( TClonesArray& tracks );
  virtual Int_t FindVertices( TClonesArray& tracks );
  virtual EStatus Init( const TDatime& date );

  // Get and Set Functions
  virtual THaVDCUVPlane* GetUpper() { return fUpper; }
  virtual THaVDCUVPlane* GetLower() { return fLower; }

  virtual Double_t GetVDCAngle() { return fVDCAngle; }
  virtual Double_t GetSpacing()  { return fSpacing;  }

  // Bits & and bit masks for THaTrack
  enum {
    kStageMask     = BIT(14) | BIT(15),  // Track processing stage bits
    kInvalid       = 0x0000,  // Not processed
    kCoarse        = BIT(14), // Coarse track
    kFine          = BIT(15), // Fine track
    kReassigned    = BIT(16), // Track is a new track in Fine stage
    kMultiTrack    = BIT(17), // Track was generated in the multitrack analysis
    kBadTrack      = BIT(18)  // Track prematurely exists the spectrometer or similar
  };

  // Bits and bit masks for this object
  enum {
    kOnlyFastest    = BIT(14), // Only use fastest hit for each wire (highest TDC)
    kTDCbits        = BIT(15) | BIT(16),  // Mask for TDC mode bits
    kHardTDCcut     = BIT(15), // Use hard TDC cuts (fMinTime, fMaxTime)
    kSoftTDCcut     = BIT(16), // Use soft TDC cut (reasonable estimated drifts)
    kIgnoreNegDrift = BIT(17), // Completely ignore negative drift times

    kCoarseOnly     = BIT(23) // Do only coarse tracking
  };

protected:

  THaVDC() {}                    // Must construct with name
  THaVDC( const THaVDC& ) {}
  THaVDC& operator=( const THaVDC& ) { return *this; }

  THaVDCUVPlane* fLower;    // Lower UV plane
  THaVDCUVPlane* fUpper;    // Upper UV plane

  TClonesArray*  fUVpairs;  // Pairs of matched UV tracks (THaVDCTrackPair obj)

  Double_t fVDCAngle;       // Angle from the VDC cs to TRANSPORT cs (rad)
  Double_t fSin_vdc;        // Sine of VDC angle
  Double_t fCos_vdc;        // Cosine of VDC angle
  Double_t fTan_vdc;        // Tangent of VDC angle
  Double_t fSpacing;        // Spacing between U1 and U2 (m)
  Int_t    fNtracks;        // Number of tracks found in ConstructTracks

  Int_t    fNumIter;        // Number of iterations for FineTrack()
  Double_t fErrorCutoff;    // Cut on track matching error

  Double_t fCentralDist;    // the path length of the central ray from
                            // the origin of the transport coordinates to 
                            // the s1 plane

  // declarations for target vertex reconstruction
  typedef enum {
    kTransport,
    kRotatingTransport
  } ECoordTypes;

  typedef enum {
    T000 = 0,
    Y000,
    P000
  } EFPMatrixElemTags;
    
  enum {
      kPORDER = 7
  };

  // private class for storing matrix element data
  class THaMatrixElement {
  public:
    THaMatrixElement() : iszero(true), pw(3), order(0), v(0), poly(kPORDER) {}
    THaMatrixElement& operator=( const THaMatrixElement& ) { return *this; }

    bool iszero;             // whether the element is zero
    vector<int> pw;          // exponents of matrix element
                             //   e.g. D100 = { 1, 0, 0 }
    int  order;
    double v;                // its computed value
    vector<double> poly;     // the associated polynomial
  };

  // initial matrix elements
  vector<THaMatrixElement> fTMatrixElems;
  vector<THaMatrixElement> fDMatrixElems;
  vector<THaMatrixElement> fPMatrixElems;
  vector<THaMatrixElement> fYMatrixElems;
  vector<THaMatrixElement> fFPMatrixElems;  // matrix elements used in
                                            // focal plane transformations
                                            // { T, Y, P }

  void CalcFocalPlaneCoords(const THaVDCUVTrack *the_track, 
			    THaTrack *new_track, const ECoordTypes mode);
  void CalcTargetCoords(THaTrack *the_track, const ECoordTypes mode);
  void CalcMatrix(const double x, vector<THaMatrixElement> &matrix);
  double DoPoly(const int n, const vector<double> &a, const double x);
  double PolyInv(const double x1, const double x2, const double xacc, 
		 const double y, const int norder, const vector<double> &a);
  double CalcTargetVar(const vector<THaMatrixElement> &matrix, 
		       const double powers[][3]);
  Int_t ReadDatabase( FILE* file, const TDatime& date );

  virtual void  Clear( Option_t* opt="" )  {}
  virtual Int_t ConstructTracks( TClonesArray * tracks = NULL, Int_t flag = 0 );

  void ProjToTransPlane(Double_t& x, Double_t& y, Double_t& z, 
			Double_t& th, Double_t& ph);

  void CorrectTimeOfFlight(TClonesArray& tracks);
  void FindBadTracks(TClonesArray &tracks);

  virtual Int_t SetupDetector( const TDatime& date );

  ClassDef(THaVDC,0)             // VDC class
}; 

////////////////////////////////////////////////////////////////////////////////

#endif
