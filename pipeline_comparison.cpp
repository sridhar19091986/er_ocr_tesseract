#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>

#include "ocr_tesseract.h"
#include "ocr_hmm_decoder.h"
#include "ergrouping_nm.h"
#include "msers_to_erstats.h"

#define REGION_TYPE        0 // 0=ERStats, 1=MSER, 2=canny+contour
#define GROUPING_ALGORITHM 1 // 0=exhaustive_search, 1=exhaustive_search + feedback loop, 2=multioriented
#define SEGMENTATION       0 // 0=B&W regions, 1=B&W regions + gaussian blur, 2=croped image, 
                             // 3=croped image + adaptive threshold, 4= cropped image + otsu threshold
                             
#define RECOGNITION        1 // 0=tesseract, 1=NM_chain_features+KNN, 2=NM_chain_features+MLP


using namespace cv;
using namespace std;

//Calculate edit distance netween two words
size_t edit_distance(const string& A, const string& B);
size_t min(size_t x, size_t y, size_t z);
bool   isRepetitive(const string& s);
bool   sort_by_lenght(const string &a, const string &b){return (a.size()>b.size());};
//Draw ER's in an image via floodFill
void   er_draw(vector<Mat> &channels, vector<vector<ERStat> > &regions, vector<Vec2i> group, Mat& segmentation);

//Perform text detection and recognition and evaluate results using edit distance
int main(int argc, char* argv[]) 
{

  Mat image;
  if(argc>1)
    image  = imread(argv[1]);
  else
  {
    cout << "Usage: " << argv[0] << " <img_filename> <gt_word1> <gt_word2> ... <gt_wordN>" << endl;
    return(0);
  }



  /*Text Detection*/

  Mat grey,orig_grey;
  cvtColor(image,grey,COLOR_RGB2GRAY);
  grey.copyTo(orig_grey);
  // Extract channels to be processed individually
  vector<Mat> channels;
  channels.push_back(grey);
  channels.push_back(255-grey);


  vector<vector<ERStat> > regions(channels.size());
  double t_d = (double)getTickCount();

  switch (REGION_TYPE)
  {
    case 0:
    {
      // ERStat
      // Create ERFilter objects with the 1st and 2nd stage default classifiers
      Ptr<ERFilter> er_filter1 = createERFilterNM1(loadClassifierNM1("trained_classifierNM1.xml"),8,0.00015,0.13,0.2,true,0.1);
      Ptr<ERFilter> er_filter2 = createERFilterNM2(loadClassifierNM2("trained_classifierNM2.xml"),0.5);
    
      // Apply the default cascade classifier to each independent channel (could be done in parallel)
      for (int c=0; c<(int)channels.size(); c++)
      {
          er_filter1->run(channels[c], regions[c]);
          er_filter2->run(channels[c], regions[c]);
      }
      break;
    }
    case 1:
    {
      //Extract MSER
      vector<vector<Point> > contours;
      MSER(21,(int)(0.00002*grey.cols*grey.rows),(int)(0.05*grey.cols*grey.rows),1,0.7)(grey, contours);
    
      //Convert the output of MSER to suitable input for the grouping/recognition algorithms
      MSERsToERStats(grey, contours, regions);
      break;
    }
    case 2:
    {
      break;
    }
  }
  cout << "TIME_REGION_DETECTION_ALT = " << ((double)getTickCount() - t_d)*1000/getTickFrequency() << endl;

  Mat out_img_decomposition= Mat::zeros(image.rows+2, image.cols+2, CV_8UC1);
  vector<Vec2i> tmp_group;
  for (int i=0; i<regions.size(); i++)
  {
    for (int j=0; j<regions[i].size();j++)
    {
      tmp_group.push_back(Vec2i(i,j));
    }
    Mat tmp= Mat::zeros(image.rows+2, image.cols+2, CV_8UC1);
    er_draw(channels, regions, tmp_group, tmp);
    if (i > 0)
      tmp = tmp / 2;
    out_img_decomposition = out_img_decomposition | tmp;
    tmp_group.clear();
  }
    
  // Detect character groups
  double t_g = getTickCount();
  vector< vector<Vec2i> > nm_region_groups;
  vector<Rect> nm_boxes;
  switch (GROUPING_ALGORITHM)
  {
    case 0:
    {
      erGroupingNM(image, channels, regions, nm_region_groups, nm_boxes, false);
      break;
    }
    case 1:
    {
      erGroupingNM(image, channels, regions, nm_region_groups, nm_boxes, true);
      break;
    }
    case 2:
    {
      erGrouping(image, channels, regions, nm_region_groups, nm_boxes, ERGROUPING_ORIENTATION_ANY, "./trained_classifier_erGrouping.xml", 0.5);
      break;
    }
  }
  cout << "TIME_GROUPING_ALT = " << ((double)getTickCount() - t_g)*1000/getTickFrequency() << endl;




  /*Text Recognition (OCR)*/

  void* ocr;
  Mat transition_p;
  Mat emission_p;
  string voc;

  double t_r = getTickCount();

  if (RECOGNITION == 0)
  {
    ocr = (void*) new OCRTesseract();
  }
  else 
  {
    transition_p = Mat(62,62,CV_64FC1);
    string filename = "transitions_OCRHMM.xml";
    FileStorage fs(filename, FileStorage::READ);
    fs["transition_probabilities"] >> transition_p;
    emission_p = Mat::eye(62,62,CV_64FC1);
    voc = "abcdefghijklmnopqrtsuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    if (RECOGNITION == 1)
    {
      ocr = (void*) new OCRHMMDecoder(loadOCRHMMClassifierKNN("ocr_hmm_decoder_train/mlp_mask/knn_model_data.xml"), 
                                      voc, transition_p, emission_p);
    }
    if (RECOGNITION == 2)
    {
      ocr = (void*) new OCRHMMDecoder(loadOCRHMMClassifierMLP("ocr_hmm_decoder_train/mlp_mask/trained_mlp.xml"), 
                                      voc, transition_p, emission_p);
    }
  }

  cout << "TIME_OCR_INITIALIZATION_ALT = "<< ((double)getTickCount() - t_r)*1000/getTickFrequency() << endl;

  Mat out_img;
  Mat out_img_detection;
  Mat out_img_segmentation = Mat::zeros(image.rows+2, image.cols+2, CV_8UC1);
  image.copyTo(out_img);
  image.copyTo(out_img_detection);
  float scale_img  = 600./image.rows;
  float scale_font = (2-scale_img)/1.4;
  vector<string> words_detection;
  string output;
 
  t_r = getTickCount();

  for (int i=0; i<nm_boxes.size(); i++)
  {

    rectangle(out_img_detection, nm_boxes[i].tl(), nm_boxes[i].br(), Scalar(0,255,255), 3);

    Mat group_img = Mat::zeros(image.rows+2, image.cols+2, CV_8UC1);
    Mat group_segmentation;
    if ((SEGMENTATION == 0)||(SEGMENTATION == 1))
    {
      er_draw(channels, regions, nm_region_groups[i], group_img);
      if (SEGMENTATION == 1)
        GaussianBlur( group_img, group_img, Size( 3, 3 ), 0, 0 );
      group_img.copyTo(group_segmentation);
      group_img(nm_boxes[i]).copyTo(group_img);
      copyMakeBorder(group_img,group_img,15,15,15,15,BORDER_CONSTANT,Scalar(0));
    } else {
      group_segmentation = Mat::zeros(image.rows+2, image.cols+2, CV_8UC1);
      Rect roi = (nm_boxes[i] + Size(10,10)) - Point(5,5);
      roi.x = max(roi.x,0); roi.y = max(roi.y,0);
      roi.width = min(image.cols-roi.x-1,roi.width); roi.height = min(image.rows-roi.y-1,roi.height);
      cout << roi.x << "," << roi.y << "," << roi.width << "," << roi.height << endl;
      orig_grey(roi).copyTo(group_img);
      if (SEGMENTATION == 3)
        adaptiveThreshold(group_img, group_img, 255, ADAPTIVE_THRESH_GAUSSIAN_C, THRESH_BINARY, 11, 2);
      if (SEGMENTATION == 4)
        threshold(group_img, group_img, 128, 255, THRESH_BINARY|THRESH_OTSU);
      group_img.copyTo(group_segmentation(roi));
    }

    vector<Rect>   boxes;
    vector<string> words;
    vector<float>  confidences;


    float min_confidence1,min_confidence2;

    if (RECOGNITION == 0)
    {
      ((OCRTesseract*)ocr)->run(group_img, output, &boxes, &words, &confidences, OCR_LEVEL_WORD);
      min_confidence1 = 51.;
      min_confidence2 = 60.;
    }
    else
    {
      ((OCRHMMDecoder*)ocr)->run(group_img, group_img, output, &boxes, &words, &confidences, OCR_LEVEL_WORD);
      min_confidence1 = 0.;
      min_confidence2 = 0.;
    }
  

    output.erase(remove(output.begin(), output.end(), '\n'), output.end());
    cout << "OCR output = \"" << output << "\" lenght = " << output.size() << endl;
    if (output.size() < 3)
      continue;

    for (int j=0; j<boxes.size(); j++)
    {
      boxes[j].x += nm_boxes[i].x-15;
      boxes[j].y += nm_boxes[i].y-15;

      //cout << "  word = " << words[j] << "\t confidence = " << confidences[j] << endl;
      if ((words[j].size() < 2) || (confidences[j] < min_confidence1) || 
          ((words[j].size()==2) && (words[j][0] == words[j][1])) ||
          ((words[j].size()< 4) && (confidences[j] < min_confidence2)) ||
          isRepetitive(words[j]))
        continue;
      words_detection.push_back(words[j]);
      rectangle(out_img, boxes[j].tl(), boxes[j].br(), Scalar(255,0,255),3);
      Size word_size = getTextSize(words[j], FONT_HERSHEY_SIMPLEX, scale_font, 3*scale_font, NULL);
      rectangle(out_img, boxes[j].tl()-Point(3,word_size.height+3), boxes[j].tl()+Point(word_size.width,0), Scalar(255,0,255),-1);
      putText(out_img, words[j], boxes[j].tl()-Point(1,1), FONT_HERSHEY_SIMPLEX, scale_font, Scalar(255,255,255),3*scale_font);
      out_img_segmentation = out_img_segmentation | group_segmentation;
    }

  }

  cout << "TIME_OCR_ALT = " << ((double)getTickCount() - t_r)*1000/getTickFrequency() << endl;


  /* Recognition evaluation with (approximate) hungarian matching and edit distances */

  if(argc>2)
  {
    int num_gt_characters   = 0;
    vector<string> words_gt;
    for (int i=2; i<argc; i++)
    {
      string s = string(argv[i]);
      if (s.size() > 0)
      {
        words_gt.push_back(string(argv[i]));
        //cout << " GT word " << words_gt[words_gt.size()-1] << endl;
        num_gt_characters += words_gt[words_gt.size()-1].size();
      }
    }

    if (words_detection.empty())
    {
      //cout << endl << "number of characters in gt = " << num_gt_characters << endl;
      cout << "TOTAL_EDIT_DISTANCE_ALT = " << num_gt_characters << endl;
      cout << "EDIT_DISTANCE_RATIO_ALT = 1" << endl;
    }
    else
    {

      sort(words_gt.begin(),words_gt.end(),sort_by_lenght);

      int max_dist=0;
      vector< vector<int> > assignment_mat;
      for (int i=0; i<words_gt.size(); i++)
      {
        vector<int> assignment_row(words_detection.size(),0);
        assignment_mat.push_back(assignment_row);
        for (int j=0; j<words_detection.size(); j++)
        {
          assignment_mat[i][j] = edit_distance(words_gt[i],words_detection[j]);
          max_dist = max(max_dist,assignment_mat[i][j]);
        }
      }
        
      vector<int> words_detection_matched;
  
      int total_edit_distance = 0;
      int assigned_gt_words=0;
      int tp=0, fp=0, fn=0; 
      for (int search_dist=0; search_dist<=max_dist; search_dist++)
      {
        for (int i=0; i<assignment_mat.size(); i++)
        {
          int min_dist_idx =  distance(assignment_mat[i].begin(),
                                       min_element(assignment_mat[i].begin(),assignment_mat[i].end()));
          if (assignment_mat[i][min_dist_idx] == search_dist)
          {
            //cout << " GT word \"" << words_gt[i] << "\" best match \"" << words_detection[min_dist_idx] << "\" with dist " << assignment_mat[i][min_dist_idx] << endl;
            if(search_dist == 0) 
		tp++;
            else { fp++; fn++; }

            total_edit_distance += assignment_mat[i][min_dist_idx];
            words_detection_matched.push_back(min_dist_idx);
            words_gt.erase(words_gt.begin()+i);
            assignment_mat.erase(assignment_mat.begin()+i);
            for (int j=0; j<assignment_mat.size(); j++)
            {
              assignment_mat[j][min_dist_idx]=INT_MAX;
            }
            i--;
          }
        }
      }
  
      for (int j=0; j<words_gt.size(); j++)
      {
        //cout << " GT word \"" << words_gt[j] << "\" no match found" << endl;
        fn++;
        total_edit_distance += words_gt[j].size();
      }
      for (int j=0; j<words_detection.size(); j++)
      {
        if (find(words_detection_matched.begin(),words_detection_matched.end(),j) == words_detection_matched.end())
        {
          //cout << " Detection word \"" << words_detection[j] << "\" no match found" << endl;
          fp++;
          total_edit_distance += words_detection[j].size();
        }
      }
  
    
      //cout << endl << "number of characters in gt = " << num_gt_characters << endl;
      cout << "TOTAL_EDIT_DISTANCE_ALT = " << total_edit_distance << endl;
      cout << "EDIT_DISTANCE_RATIO_ALT = " << (float)total_edit_distance / num_gt_characters << endl;
      cout << "TP_ALT = " << tp << endl;
      cout << "FP_ALT = " << fp << endl;
      cout << "FN_ALT = " << fn << endl;
    }
  }



  //resize(out_img_detection,out_img_detection,Size(image.cols*scale_img,image.rows*scale_img));
  //imshow("detection", out_img_detection);
  imwrite("detection_alt.jpg", out_img_detection);
  //resize(out_img,out_img,Size(image.cols*scale_img,image.rows*scale_img));
  //imshow("recognition", out_img);
  imwrite("recognition_alt.jpg", out_img);
  //waitKey(0);
  imwrite("segmentation_alt.jpg", out_img_segmentation);
  imwrite("decomposition_alt.jpg", out_img_decomposition);

  return 0;
}

size_t min(size_t x, size_t y, size_t z)
{
  return x < y ? min(x,z) : min(y,z);
}

size_t edit_distance(const string& A, const string& B)
{
  size_t NA = A.size();
  size_t NB = B.size();

  vector< vector<size_t> > M(NA + 1, vector<size_t>(NB + 1));

  for (size_t a = 0; a <= NA; ++a)
    M[a][0] = a;

  for (size_t b = 0; b <= NB; ++b)
    M[0][b] = b;

  for (size_t a = 1; a <= NA; ++a)
    for (size_t b = 1; b <= NB; ++b)
    {
      size_t x = M[a-1][b] + 1;
      size_t y = M[a][b-1] + 1;
      size_t z = M[a-1][b-1] + (A[a-1] == B[b-1] ? 0 : 1);
      M[a][b] = min(x,y,z);
    }

  return M[A.size()][B.size()];
}

bool isRepetitive(const string& s)
{
  int count  = 0;
  int count2 = 0;
  int count3 = 0;
  int first=(int)s[0];
  int last=(int)s[s.size()-1];
  for (int i=0; i<s.size(); i++)
  {
    if ((s[i] == 'i') ||
        (s[i] == 'l') ||
        (s[i] == 'I'))
      count++;
    if((int)s[i]==first)
      count2++;
    if((int)s[i]==last)
      count3++;
  }
  if ((count > (s.size()+1)/2) || (count2 == s.size()) || (count3 > (s.size()*2)/3))
  {
    return true;
  }


  return false;
}


void er_draw(vector<Mat> &channels, vector<vector<ERStat> > &regions, vector<Vec2i> group, Mat& segmentation)
{
  for (int r=0; r<(int)group.size(); r++)
  {
      ERStat er = regions[group[r][0]][group[r][1]];
      if (er.parent != NULL) // deprecate the root region
      {
          int newMaskVal = 255;
          int flags = 4 + (newMaskVal << 8) + FLOODFILL_FIXED_RANGE + FLOODFILL_MASK_ONLY;
          floodFill(channels[group[r][0]],segmentation,Point(er.pixel%channels[group[r][0]].cols,er.pixel/channels[group[r][0]].cols),
                    Scalar(255),0,Scalar(er.level),Scalar(0),flags);
      }
  }
}
