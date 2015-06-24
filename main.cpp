
// @@@ imgproc gpu algo does not support mipmapping binding yet
// @@@ test compute shader group size
// @@@ ADD OPENGL HARDWARE LIMITS CHECKS EVERYWHERE (if using ssbo, check MAX_SHADER_STORAGE_BLOCK_SIZE)
// @@@ always pack big gpu buffers with std430
// @@@ add opencv hardware intrs check, and impl some stuff in MMX/SSE/SSE2/3/4.1/4.2? (also check popcount and AVX)
// @@@ investigate glClearBufferData for large-scale opengl buffer memsets
// @@@ support non-integer textures top level (alg)? need to replace all ui-stores by float-stores, rest is ok

//////////////////////////////////////////
// USER/ENVIRONMENT-SPECIFIC VARIABLES :
//////////////////////////////////////////
#define EVAL_RESULTS_ONLY                0
#define WRITE_BGSUB_IMG_OUTPUT           0
#define WRITE_BGSUB_DEBUG_IMG_OUTPUT     0
#define WRITE_BGSUB_METRICS_ANALYSIS     0
#define DISPLAY_BGSUB_DEBUG_OUTPUT       1
#define ENABLE_TIMERS                    0
#define ENABLE_DISPLAY_MOUSE_DEBUG       0
#define WRITE_BGSUB_SEGM_AVI_OUTPUT      0
//////////////////////////////////////////
#define USE_VIBE_BGSUB                   0
#define USE_PBAS_BGSUB                   0
#define USE_PAWCS_BGSUB                  0
#define USE_LOBSTER_BGSUB                1
#define USE_SUBSENSE_BGSUB               0
//////////////////////////////////////////

#include "ParallelUtils.h"
#include "DatasetUtils.h"
#define DATASET_ID             DatasetUtils::eDataset_CDnet2014
#define DATASET_ROOT_PATH      std::string("/shared2/datasets/")
#define DATASET_RESULTS_PATH   std::string("results")

#define LIMIT_MODEL_TO_SEQUENCE_ROI (USE_LOBSTER_BGSUB||USE_SUBSENSE_BGSUB||USE_PAWCS_BGSUB)
#define BOOTSTRAP_100_FIRST_FRAMES  (USE_LOBSTER_BGSUB||USE_SUBSENSE_BGSUB||USE_PAWCS_BGSUB)
#if EVAL_RESULTS_ONLY && (DEFAULT_NB_THREADS>1 || HAVE_GPU_SUPPORT || !WRITE_BGSUB_METRICS_ANALYSIS)
#error "Eval-only mode must run on CPU with 1 thread & write results somewhere."
#endif //EVAL_RESULTS_ONLY && (DEFAULT_NB_THREADS>1 || !WRITE_BGSUB_METRICS_ANALYSIS)
#if (USE_LOBSTER_BGSUB+USE_SUBSENSE_BGSUB+USE_VIBE_BGSUB+USE_PBAS_BGSUB+USE_PAWCS_BGSUB)!=1
#error "Must specify a single algorithm."
#elif USE_VIBE_BGSUB
#include "BackgroundSubtractorViBe_1ch.h"
#include "BackgroundSubtractorViBe_3ch.h"
#elif USE_PBAS_BGSUB
#include "BackgroundSubtractorPBAS_1ch.h"
#include "BackgroundSubtractorPBAS_3ch.h"
#elif USE_PAWCS_BGSUB
#include "BackgroundSubtractorPAWCS.h"
#elif USE_LOBSTER_BGSUB
#include "BackgroundSubtractorLOBSTER.h"
#elif USE_SUBSENSE_BGSUB
#include "BackgroundSubtractorSuBSENSE.h"
#endif //USE_..._BGSUB
#if ENABLE_DISPLAY_MOUSE_DEBUG
#if (HAVE_GPU_SUPPORT || DEFAULT_NB_THREADS>1)
#error "Cannot support mouse debug with GPU support or with more than one thread."
#endif //(HAVE_GPU_SUPPORT || DEFAULT_NB_THREADS>1)
static int *pnLatestMouseX=nullptr, *pnLatestMouseY=nullptr;
void OnMouseEvent(int event, int x, int y, int, void*) {
    if(event!=cv::EVENT_MOUSEMOVE || !x || !y)
        return;
    *pnLatestMouseX = x;
    *pnLatestMouseY = y;
}
#endif //ENABLE_DISPLAY_MOUSE_DEBUG
#if WRITE_BGSUB_METRICS_ANALYSIS
cv::FileStorage g_oDebugFS;
#endif //!WRITE_BGSUB_METRICS_ANALYSIS
#if ENABLE_TIMERS
enum eCPUTimersList {
    eCPUTimer_VideoQuery=0,
    eCPUTimer_PipelineUpdate,
    eCPUTimer_OverallLoop,
    eCPUTimersCount
};
#endif //ENABLE_TIMERS
#if WRITE_BGSUB_IMG_OUTPUT
const std::vector<int> g_vnResultsComprParams = {cv::IMWRITE_PNG_COMPRESSION,9}; // when writing output bin files, lower to increase processing speed
#endif //WRITE_BGSUB_IMG_OUTPUT
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
cv::Size g_oDisplayOutputSize(960,240);
bool g_bContinuousUpdates = false;
#endif //(DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
const DatasetUtils::DatasetInfo& g_oDatasetInfo = DatasetUtils::GetDatasetInfo(DATASET_ID,DATASET_ROOT_PATH,DATASET_RESULTS_PATH);
#if (HAVE_GPU_SUPPORT && DEFAULT_NB_THREADS>1)
#warning "Cannot support multithreading + gpu exec, will keep one main thread + gpu instead"
#endif //(HAVE_GPU_SUPPORT && DEFAULT_NB_THREADS>1)
int AnalyzeSequence(int nThreadIdx, std::shared_ptr<DatasetUtils::SequenceInfo> pCurrSequence, const std::string& sCurrResultsPath);
#if !HAVE_GPU_SUPPORT
const size_t g_nMaxThreads = DEFAULT_NB_THREADS;//std::thread::hardware_concurrency()>0?std::thread::hardware_concurrency():DEFAULT_NB_THREADS;
std::atomic_size_t g_nActiveThreads(0);
#endif //!HAVE_GPU_SUPPORT

int main() {
#if PLATFORM_USES_WIN32API
    SetConsoleWindowSize(80,40,1000);
#endif //PLATFORM_USES_WIN32API
    std::cout << "Parsing dataset..." << std::endl;
    std::vector<std::shared_ptr<DatasetUtils::CategoryInfo>> vpCategories;
    for(auto oDatasetFolderPathIter=g_oDatasetInfo.vsDatasetFolderPaths.begin(); oDatasetFolderPathIter!=g_oDatasetInfo.vsDatasetFolderPaths.end(); ++oDatasetFolderPathIter) {
        try {
            vpCategories.push_back(std::make_shared<DatasetUtils::CategoryInfo>(*oDatasetFolderPathIter,g_oDatasetInfo.sDatasetPath+*oDatasetFolderPathIter,g_oDatasetInfo.eID,g_oDatasetInfo.vsDatasetGrayscaleDirPathTokens,g_oDatasetInfo.vsDatasetSkippedDirPathTokens,HAVE_GPU_SUPPORT));
        } catch(std::runtime_error& e) { std::cout << e.what() << std::endl; }
    }
    size_t nSeqTotal = 0;
    size_t nFramesTotal = 0;
    std::multimap<double,std::shared_ptr<DatasetUtils::SequenceInfo>> mSeqLoads;
    for(auto pCurrCategory=vpCategories.begin(); pCurrCategory!=vpCategories.end(); ++pCurrCategory) {
        nSeqTotal += (*pCurrCategory)->m_vpSequences.size();
        for(auto pCurrSequence=(*pCurrCategory)->m_vpSequences.begin(); pCurrSequence!=(*pCurrCategory)->m_vpSequences.end(); ++pCurrSequence) {
            nFramesTotal += (*pCurrSequence)->GetNbInputFrames();
            mSeqLoads.insert(std::make_pair((*pCurrSequence)->m_dExpectedROILoad,(*pCurrSequence)));
        }
    }
    CV_Assert(mSeqLoads.size()==nSeqTotal);
    std::cout << "Parsing complete. [" << vpCategories.size() << " category(ies), "  << nSeqTotal  << " sequence(s)]" << std::endl << std::endl;
    if(nSeqTotal) {
        size_t nSeqProcessed = 1;
        const std::string sCurrResultsPath = g_oDatasetInfo.sResultsPath;
#if WRITE_BGSUB_METRICS_ANALYSIS
        g_oDebugFS = cv::FileStorage(sCurrResultsPath+"framelevel_debug.yml",cv::FileStorage::WRITE);
#endif //WRITE_BGSUB_METRICS_ANALYSIS
#if EVAL_RESULTS_ONLY
        std::cout << "Executing background subtraction evaluation..." << std::endl;
        for(auto oSeqIter=mSeqLoads.rbegin(); oSeqIter!=mSeqLoads.rend(); ++oSeqIter) {
            std::cout << "\tProcessing [" << nSeqProcessed << "/" << nSeqTotal << "] (" << oSeqIter->second->m_pParent->m_sName << ":" << oSeqIter->second->m_sName << ", L=" << std::scientific << std::setprecision(2) << oSeqIter->first << ")" << std::endl;
            for(size_t nCurrFrameIdx=0; nCurrFrameIdx<oSeqIter->second->GetNbGTFrames(); ++nCurrFrameIdx) {
                cv::Mat oGTImg = oSeqIter->second->GetGTFrameFromIndex(nCurrFrameIdx);
                cv::Mat oFGMask = DatasetUtils::ReadResult(sCurrResultsPath,oSeqIter->second->m_pParent->m_sName,oSeqIter->second->m_sName,g_oDatasetInfo.sResultPrefix,nCurrFrameIdx+g_oDatasetInfo.nResultIdxOffset,g_oDatasetInfo.sResultSuffix);
                DatasetUtils::CalcMetricsFromResult(oFGMask,oGTImg,oSeqIter->second->GetSequenceROI(),oSeqIter->second->nTP,oSeqIter->second->nTN,oSeqIter->second->nFP,oSeqIter->second->nFN,oSeqIter->second->nSE);
            }
            ++nSeqProcessed;
        }
        const double dFinalFPS = 0.0;
#else //!EVAL_RESULTS_ONLY
        PlatformUtils::CreateDirIfNotExist(sCurrResultsPath);
        for(size_t c=0; c<vpCategories.size(); ++c)
            PlatformUtils::CreateDirIfNotExist(sCurrResultsPath+vpCategories[c]->m_sName+"/");
        time_t nStartupTime = time(nullptr);
        const std::string sStartupTimeStr(asctime(localtime(&nStartupTime)));
        std::cout << "[" << sStartupTimeStr.substr(0,sStartupTimeStr.size()-1) << "]" << std::endl;
#if !HAVE_GPU_SUPPORT && DEFAULT_NB_THREADS>1
        std::cout << "Executing background subtraction with " << ((g_nMaxThreads>nSeqTotal)?nSeqTotal:g_nMaxThreads) << " thread(s)..." << std::endl;
#else //DEFAULT_NB_THREADS==1
        std::cout << "Executing background subtraction..." << std::endl;
#endif //DEFAULT_NB_THREADS==1
#if HAVE_GPU_SUPPORT
        for(auto oSeqIter=mSeqLoads.rbegin(); oSeqIter!=mSeqLoads.rend(); ++oSeqIter) {
            std::cout << "\tProcessing [" << nSeqProcessed << "/" << nSeqTotal << "] (" << oSeqIter->second->m_pParent->m_sName << ":" << oSeqIter->second->m_sName << ", L=" << std::scientific << std::setprecision(2) << oSeqIter->first << ")" << std::endl;
            ++nSeqProcessed;
            AnalyzeSequence(0,oSeqIter->second,sCurrResultsPath);
        }
#else //!HAVE_GPU_SUPPORT
        for(auto oSeqIter=mSeqLoads.rbegin(); oSeqIter!=mSeqLoads.rend(); ++oSeqIter) {
            while(g_nActiveThreads>=g_nMaxThreads)
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::cout << "\tProcessing [" << nSeqProcessed << "/" << nSeqTotal << "] (" << oSeqIter->second->m_pParent->m_sName << ":" << oSeqIter->second->m_sName << ", L=" << std::scientific << std::setprecision(2) << oSeqIter->first << ")" << std::endl;
            ++g_nActiveThreads;
            ++nSeqProcessed;
            std::thread(AnalyzeSequence,nSeqProcessed,oSeqIter->second,sCurrResultsPath).detach();
        }
        while(g_nActiveThreads>0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#endif //!HAVE_GPU_SUPPORT
        time_t nShutdownTime = time(nullptr);
        const double dFinalFPS = (nShutdownTime-nStartupTime)>0?(double)nFramesTotal/(nShutdownTime-nStartupTime):std::numeric_limits<double>::quiet_NaN();
        std::cout << "Execution completed; " << nFramesTotal << " frames over " << (nShutdownTime-nStartupTime) << " sec = " << std::fixed << std::setprecision(1) << dFinalFPS << " FPS overall" << std::endl;
        const std::string sShutdownTimeStr(asctime(localtime(&nShutdownTime)));
        std::cout << "[" << sShutdownTimeStr.substr(0,sShutdownTimeStr.size()-1) << "]\n" << std::endl;
#endif //!EVAL_RESULTS_ONLY
#if WRITE_BGSUB_METRICS_ANALYSIS
        std::cout << "Summing and writing metrics results..." << std::endl;
        for(size_t c=0; c<vpCategories.size(); ++c) {
            if(!vpCategories[c]->m_vpSequences.empty()) {
                for(size_t s=0; s<vpCategories[c]->m_vpSequences.size(); ++s) {
                    vpCategories[c]->nTP += vpCategories[c]->m_vpSequences[s]->nTP;
                    vpCategories[c]->nTN += vpCategories[c]->m_vpSequences[s]->nTN;
                    vpCategories[c]->nFP += vpCategories[c]->m_vpSequences[s]->nFP;
                    vpCategories[c]->nFN += vpCategories[c]->m_vpSequences[s]->nFN;
                    vpCategories[c]->nSE += vpCategories[c]->m_vpSequences[s]->nSE;
                    DatasetUtils::WriteMetrics(sCurrResultsPath+vpCategories[c]->m_sName+"/"+vpCategories[c]->m_vpSequences[s]->m_sName+".txt",vpCategories[c]->m_vpSequences[s]);
                }
                DatasetUtils::WriteMetrics(sCurrResultsPath+vpCategories[c]->m_sName+".txt",vpCategories[c]);
                std::cout << std::endl;
            }
        }
        DatasetUtils::WriteMetrics(sCurrResultsPath+"METRICS_TOTAL.txt",vpCategories,dFinalFPS);
        g_oDebugFS.release();
#endif //WRITE_BGSUB_METRICS_ANALYSIS
        std::cout << "All done." << std::endl;
    }
    else
        std::cout << "No sequences found, all done." << std::endl;
}

int AnalyzeSequence(int nThreadIdx, std::shared_ptr<DatasetUtils::SequenceInfo> pCurrSequence, const std::string& sCurrResultsPath) {
    srand(0); // for now, assures that two consecutive runs on the same data return the same results
    //srand((unsigned int)time(NULL));
#if USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB
    std::unique_ptr<BackgroundSubtractorLBSP> pBGS;
#if LIMIT_MODEL_TO_SEQUENCE_ROI
    cv::Mat oSequenceROI = pCurrSequence->GetSequenceROI();
#else //!LIMIT_MODEL_TO_SEQUENCE_ROI
    cv::Mat oSequenceROI;
#endif //!LIMIT_MODEL_TO_SEQUENCE_ROI
#else //USE_VIBE_BGSUB || USE_PBAS_BGSUB
    std::unique_ptr<cv::BackgroundSubtractor> pBGS;
#endif //USE_VIBE_BGSUB || USE_PBAS_BGSUB
#if HAVE_GLSL
    bool bContextInitialized = false;
#endif //HAVE_GLSL
    try {
        CV_Assert(pCurrSequence && pCurrSequence->GetNbInputFrames()>1);
#if DATASETUTILS_USE_PRECACHED_IO
        pCurrSequence->StartPrecaching();
#endif //DATASETUTILS_USE_PRECACHED_IO
        cv::Mat oInitImg = pCurrSequence->GetInputFrameFromIndex(0);
#if (WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
        cv::Mat oFGMask;
#endif //(WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
        CV_Assert(!oInitImg.empty() && oInitImg.isContinuous());
#if HAVE_GLSL
        glAssert(oInitImg.channels()==1 || oInitImg.channels()==4);
        cv::Size oCurrInputSize,oCurrWindowSize;
        oCurrInputSize = oCurrWindowSize = oInitImg.size();
        // note: never construct GL classes before context initialization
        if(glfwInit()==GL_FALSE)
            glError("Failed to init GLFW");
        bContextInitialized = true;
        glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,4);
        glfwWindowHint(GLFW_RESIZABLE,GL_FALSE);
#if !GPU_RENDERING
        glfwWindowHint(GLFW_VISIBLE,GL_FALSE);
#endif //!GPU_RENDERING
        std::unique_ptr<GLFWwindow,void(*)(GLFWwindow*)> pWindow(glfwCreateWindow(oCurrWindowSize.width,oCurrWindowSize.height,"changedet_gpu",nullptr,nullptr),glfwDestroyWindow);
        if(!pWindow)
            glError("Failed to create window via GLFW");
        glfwMakeContextCurrent(pWindow.get());
        GLenum eErrCode;
        glewExperimental = GL_TRUE;
        glErrorCheck;
        if((eErrCode=glewInit())!=GLEW_OK)
            glError2(eErrCode,"Failed to init GLEW");
        else if((eErrCode=glGetError())!=GL_INVALID_ENUM)
            glError2(eErrCode,"unexpected GLEW init error code");
        if(!glewIsSupported("GL_VERSION_4_4"))
            glError("Bad GL core/ext version detected");
        if(!glGetTextureSubImage)
            std::cout << "\n\tWarning: glGetTextureSubImage not supported, performance might be affected\n" << std::endl;
#endif //HAVE_GLSL
#if USE_LOBSTER_BGSUB
        pBGS = std::unique_ptr<BackgroundSubtractorLBSP>(new BackgroundSubtractorLOBSTER());
        const double dDefaultLearningRate = BGSLOBSTER_DEFAULT_LEARNING_RATE;
        pBGS->initialize(oInitImg,oSequenceROI);
#elif USE_SUBSENSE_BGSUB
        pBGS = std::unique_ptr<BackgroundSubtractorLBSP>(new BackgroundSubtractorSuBSENSE());
        const double dDefaultLearningRate = 0;
        pBGS->initialize(oInitImg,oSequenceROI);
#elif USE_PAWCS_BGSUB
        pBGS = std::unique_ptr<BackgroundSubtractorLBSP>(new BackgroundSubtractorPAWCS());
        const double dDefaultLearningRate = 0;
        pBGS->initialize(oInitImg,oSequenceROI);
#else //USE_VIBE_BGSUB || USE_PBAS_BGSUB
        const size_t m_nInputChannels = (size_t)oInitImg.channels();
#if USE_VIBE_BGSUB
        if(m_nInputChannels==3)
            pBGS = std::unique_ptr<cv::BackgroundSubtractor>(new BackgroundSubtractorViBe_3ch());
        else
            pBGS = std::unique_ptr<cv::BackgroundSubtractor>(new BackgroundSubtractorViBe_1ch());
        ((BackgroundSubtractorViBe*)pBGS.get())->initialize(oInitImg);
        const double dDefaultLearningRate = BGSVIBE_DEFAULT_LEARNING_RATE;
#else //USE_PBAS_BGSUB
        if(m_nInputChannels==3)
            pBGS = std::unique_ptr<cv::BackgroundSubtractor>(new BackgroundSubtractorPBAS_3ch());
        else
            pBGS = std::unique_ptr<cv::BackgroundSubtractor>(new BackgroundSubtractorPBAS_1ch());
        ((BackgroundSubtractorPBAS*)pBGS.get())->initialize(oInitImg);
        const double dDefaultLearningRate = BGSPBAS_DEFAULT_LEARNING_RATE_OVERRIDE;
#endif //USE_PBAS_BGSUB
#endif //USE_VIBE_BGSUB || USE_PBAS_BGSUB
#if USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB
        pBGS->m_sDebugName = pCurrSequence->m_pParent->m_sName+"_"+pCurrSequence->m_sName;
#if WRITE_BGSUB_METRICS_ANALYSIS
        pBGS->m_pDebugFS = &g_oDebugFS;
#endif //WRITE_BGSUB_METRICS_ANALYSIS
#if ENABLE_DISPLAY_MOUSE_DEBUG
        pnLatestMouseX = &pBGS->m_nDebugCoordX;
        pnLatestMouseY = &pBGS->m_nDebugCoordY;
#endif //ENABLE_DISPLAY_MOUSE_DEBUG
#endif //USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB
#if DISPLAY_BGSUB_DEBUG_OUTPUT
        std::string sDebugDisplayName = pCurrSequence->m_pParent->m_sName + std::string(" -- ") + pCurrSequence->m_sName;
        cv::namedWindow(sDebugDisplayName);
#endif //DISPLAY_BGSUB_DEBUG_OUTPUT
        CV_Assert((!WRITE_BGSUB_DEBUG_IMG_OUTPUT && !WRITE_BGSUB_SEGM_AVI_OUTPUT && !WRITE_BGSUB_IMG_OUTPUT) || !sCurrResultsPath.empty());
#if ENABLE_DISPLAY_MOUSE_DEBUG
        std::string sMouseDebugDisplayName = pCurrSequence->m_pParent->m_sName + std::string(" -- ") + pCurrSequence->m_sName + " [MOUSE DEBUG]";
        cv::namedWindow(sMouseDebugDisplayName,0);
        cv::setMouseCallback(sMouseDebugDisplayName,OnMouseEvent,nullptr);
#endif //ENABLE_DISPLAY_MOUSE_DEBUG
#if WRITE_BGSUB_DEBUG_IMG_OUTPUT
        cv::VideoWriter oDebugWriter(sCurrResultsPath+pCurrSequence->m_pParent->m_sName+"/"+pCurrSequence->m_sName+".avi",CV_FOURCC('X','V','I','D'),30,g_oDisplayOutputSize,true);
#endif //WRITE_BGSUB_DEBUG_IMG_OUTPUT
#if WRITE_BGSUB_SEGM_AVI_OUTPUT
        cv::VideoWriter oSegmWriter(sCurrResultsPath+pCurrSequence->m_pParent->m_sName+"/"+pCurrSequence->m_sName+"_segm.avi",CV_FOURCC('F','F','V','1'),30,pCurrSequence->GetSize(),false);
#endif //WRITE_BGSUB_SEGM_AVI_OUTPUT
#if WRITE_BGSUB_IMG_OUTPUT
        PlatformUtils::CreateDirIfNotExist(sCurrResultsPath+pCurrSequence->m_pParent->m_sName+"/"+pCurrSequence->m_sName+"/");
#endif //WRITE_BGSUB_IMG_OUTPUT
#if HAVE_GLSL
        if(GLImageProcAlgo* pImgProcAlgo = dynamic_cast<GLImageProcAlgo*>(pBGS.get())) {
            oCurrWindowSize.width *= pImgProcAlgo->m_nSxSDisplayCount;
            glfwSetWindowSize(pWindow.get(),oCurrWindowSize.width,oCurrWindowSize.height);
            glViewport(0,0,oCurrWindowSize.width,oCurrWindowSize.height);
#if (WRITE_BGSUB_METRICS_ANALYSIS || DISPLAY_BGSUB_DEBUG_OUTPUT)
            pImgProcAlgo->setOutputFetching(true);
#if DISPLAY_BGSUB_DEBUG_OUTPUT
            pImgProcAlgo->setDebugFetching(true);
#endif //DISPLAY_BGSUB_DEBUG_OUTPUT
#endif //(WRITE_BGSUB_METRICS_ANALYSIS || DISPLAY_BGSUB_DEBUG_OUTPUT)
        }
        else
            glError("BGSub algorithm has no GLImageProcAlgo interface");
#endif //HAVE_GLSL
        const double dCPUTickFreq_MS = cv::getTickFrequency()/1000;
        const int64 nCPUTimerInitVal = cv::getTickCount();
#if ENABLE_TIMERS
        int64 nCPUTimerVals[eCPUTimersCount];
        double dCPUTimerTotValSum = 0;
#endif //ENABLE_TIMERS
        const std::string sCurrSeqName = pCurrSequence->m_sName.size()>12?pCurrSequence->m_sName.substr(0,12):pCurrSequence->m_sName;
        const size_t nInputFrameCount = pCurrSequence->GetNbInputFrames();
        for(size_t nCurrFrameIdx=0;; nCurrFrameIdx++) {
            if(!(nCurrFrameIdx%100))
                std::cout << "\t\t" << std::setfill(' ') << std::setw(12) << sCurrSeqName << " @ F:" << std::setfill('0') << std::setw(PlatformUtils::decimal_integer_digit_count((int)nInputFrameCount)) << nCurrFrameIdx << "/" << nInputFrameCount << "   [T=" << nThreadIdx << "]" << std::endl;
#if ASYNC_PROCESS
            if(nCurrFrameIdx==nInputFrameCount+1)
                break;
            const size_t nInputFrameQueryIdx = (nCurrFrameIdx==nInputFrameCount)?nCurrFrameIdx-1:nCurrFrameIdx;
#else //!ASYNC_PROCESS
            if(nCurrFrameIdx==nInputFrameCount)
                break;
            const size_t nInputFrameQueryIdx = nCurrFrameIdx;
#endif //ASYNC_PROCESS
#if HAVE_GLSL
            if(glfwWindowShouldClose(pWindow.get()))
                break;
            glfwPollEvents();
#if GPU_RENDERING
            if(glfwGetKey(pWindow, GLFW_KEY_ESCAPE) || glfwGetKey(pWindow, GLFW_KEY_Q))
                break;
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif //GPU_RENDERING
#endif //HAVE_GLSL
#if ENABLE_TIMERS
            int64 nCPUTimerTick_OverallLoop = cv::getTickCount();
            int64 nCPUTimerTick_VideoQuery = cv::getTickCount();
#endif //ENABLE_TIMERS
            const cv::Mat& oInputImg = pCurrSequence->GetInputFrameFromIndex(nInputFrameQueryIdx);
#if ENABLE_DISPLAY_MOUSE_DEBUG
            cv::imshow(sMouseDebugDisplayName,oInputImg);
#endif //ENABLE_DISPLAY_MOUSE_DEBUG
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
            cv::Mat oLastBGImg;
            pBGS->getBackgroundImage(oLastBGImg);
#if (USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB)
            if(!oSequenceROI.empty())
                cv::bitwise_or(oLastBGImg,UCHAR_MAX/2,oLastBGImg,oSequenceROI==0);
#endif //(USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB)
#endif //(DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
#if ENABLE_TIMERS
            nCPUTimerVals[eCPUTimer_VideoQuery] = cv::getTickCount()-nCPUTimerTick_VideoQuery;
            int64 nCPUTimerTick_PipelineUpdate = cv::getTickCount();
#endif //ENABLE_TIMERS
#if (HAVE_GLSL && ASYNC_PROCESS)
#if (WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
            pBGS->apply_async(oInputImg,oFGMask,(BOOTSTRAP_100_FIRST_FRAMES&&nCurrFrameIdx<=100)?1:dDefaultLearningRate);
#else //!(WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
            pBGS->apply_async(oInputImg,(BOOTSTRAP_100_FIRST_FRAMES&&nCurrFrameIdx<=100)?1:dDefaultLearningRate);
#endif //!(WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
#else //!(HAVE_GLSL && ASYNC_PROCESS)
            pBGS->apply(oInputImg,oFGMask,(BOOTSTRAP_100_FIRST_FRAMES&&nCurrFrameIdx<=100)?1:dDefaultLearningRate);
#endif //!(HAVE_GLSL && ASYNC_PROCESS)
#if ENABLE_TIMERS
            nCPUTimerVals[eCPUTimer_PipelineUpdate] = cv::getTickCount()-nCPUTimerTick_PipelineUpdate;
#endif //ENABLE_TIMERS
#if HAVE_GLSL
            glErrorCheck;
#if GPU_RENDERING
            glfwSwapBuffers(pWindow);
#endif //GPU_RENDERING
#endif //HAVE_GLSL
#if ASYNC_PROCESS
            if(nCurrFrameIdx>0) {
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS)
                const size_t nGTFrameQueryIdx = nCurrFrameIdx-1;
#endif //DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS
#else //!ASYNC_PROCESS
            {
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS)
                const size_t nGTFrameQueryIdx = nCurrFrameIdx;
#endif //(DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS)
#endif //!ASYNC_PROCESS
#if (WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
#if (USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB)
                if(!oSequenceROI.empty())
                    cv::bitwise_or(oFGMask,UCHAR_MAX/2,oFGMask,oSequenceROI==0);
#endif //(USE_LOBSTER_BGSUB || USE_SUBSENSE_BGSUB || USE_PAWCS_BGSUB)
#endif //(WRITE_BGSUB_IMG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_SEGM_AVI_OUTPUT)
#if WRITE_BGSUB_SEGM_AVI_OUTPUT
                oSegmWriter.write(oFGMask);
#endif //WRITE_BGSUB_SEGM_AVI_OUTPUT
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS)
                cv::Mat oGTImg = pCurrSequence->GetGTFrameFromIndex(nGTFrameQueryIdx);
#endif //(DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT || WRITE_BGSUB_METRICS_ANALYSIS)
#if (DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
#if ENABLE_DISPLAY_MOUSE_DEBUG
                cv::Mat oDebugDisplayFrame = DatasetUtils::GetDisplayResult(oInputImg,oLastBGImg,oFGMask,oGTImg,pCurrSequence->GetSequenceROI(),nGTFrameQueryIdx,(pnLatestMouseX&&pnLatestMouseY)?cv::Point(*pnLatestMouseX,*pnLatestMouseY):cv::Point(-1,-1));
#else //!ENABLE_DISPLAY_MOUSE_DEBUG
                cv::Mat oDebugDisplayFrame = DatasetUtils::GetDisplayResult(oInputImg,oLastBGImg,oFGMask,oGTImg,pCurrSequence->GetSequenceROI(),nGTFrameQueryIdx);
#endif //!ENABLE_DISPLAY_MOUSE_DEBUG
#if WRITE_BGSUB_DEBUG_IMG_OUTPUT
                oDebugWriter.write(oDebugDisplayFrame);
#endif //WRITE_BGSUB_DEBUG_IMG_OUTPUT
#if DISPLAY_BGSUB_DEBUG_OUTPUT
                cv::Mat oDebugDisplayFrameResized;
                if(oDebugDisplayFrame.cols>1280 || oDebugDisplayFrame.rows>960)
                    cv::resize(oDebugDisplayFrame,oDebugDisplayFrameResized,cv::Size(oDebugDisplayFrame.cols/2,oDebugDisplayFrame.rows/2));
                else
                    oDebugDisplayFrameResized = oDebugDisplayFrame;
                cv::imshow(sDebugDisplayName,oDebugDisplayFrameResized);
                int nKeyPressed;
                if(g_bContinuousUpdates)
                    nKeyPressed = cv::waitKey(1);
                else
                    nKeyPressed = cv::waitKey(0);
                if(nKeyPressed!=-1) {
                    nKeyPressed %= (UCHAR_MAX+1); // fixes return val bug in some opencv versions
                    std::cout << "nKeyPressed = " << nKeyPressed%(UCHAR_MAX+1) << std::endl;
                }
                if(nKeyPressed==' ')
                    g_bContinuousUpdates = !g_bContinuousUpdates;
                else if(nKeyPressed==(int)'q')
                    break;
#endif //DISPLAY_BGSUB_DEBUG_OUTPUT
#endif //(DISPLAY_BGSUB_DEBUG_OUTPUT || WRITE_BGSUB_DEBUG_IMG_OUTPUT)
#if WRITE_BGSUB_IMG_OUTPUT
                DatasetUtils::WriteResult(sCurrResultsPath,pCurrSequence->m_pParent->m_sName,pCurrSequence->m_sName,g_oDatasetInfo.sResultPrefix,nGTFrameQueryIdx+g_oDatasetInfo.nResultIdxOffset,g_oDatasetInfo.sResultSuffix,oFGMask,g_vnResultsComprParams);
#endif //WRITE_BGSUB_IMG_OUTPUT
#if WRITE_BGSUB_METRICS_ANALYSIS
                DatasetUtils::CalcMetricsFromResult(oFGMask,oGTImg,pCurrSequence->GetSequenceROI(),pCurrSequence->nTP,pCurrSequence->nTN,pCurrSequence->nFP,pCurrSequence->nFN,pCurrSequence->nSE);
#endif //WRITE_BGS
            }
#if IMGPROC_MICRO_TIME
            nCPUTimerVals[eCPUTimer_OverallLoop] = cv::getTickCount()-nCPUTimerTick_OverallLoop;
            std::cout << "\t\tCPU: ";
            std::cout << "VideoQuery=" << nCPUTimerVals[eCPUTimer_VideoQuery]/dCPUTickFreq_MS << "ms,  ";
            std::cout << "PipelineUpdate=" << nCPUTimerVals[eCPUTimer_PipelineUpdate]/dCPUTickFreq_MS << "ms,  ";
            double dCurrCPUTimerTotVal = nCPUTimerVals[eCPUTimer_OverallLoop]/dCPUTickFreq_MS;
            std::cout << " tot=" << dCurrCPUTimerTotVal << "ms\n" << std::endl;
#endif //IMGPROC_MICRO_TIME
        }
        const double dTimeElapsed_MS = double(cv::getTickCount()-nCPUTimerInitVal)/dCPUTickFreq_MS;
        const double dAvgFPS = (double)nInputFrameCount/(dTimeElapsed_MS/1000);
        std::cout << "\t\t" << std::setfill(' ') << std::setw(12) << sCurrSeqName << " @ end, " << (int)(dTimeElapsed_MS/1000) << " sec (" << (int)floor(dAvgFPS+0.5) << " FPS)" << std::endl;
#if WRITE_BGSUB_METRICS_ANALYSIS
        pCurrSequence->m_dAvgFPS = dAvgFPS;
        DatasetUtils::WriteMetrics(sCurrResultsPath+pCurrSequence->m_pParent->m_sName+"/"+pCurrSequence->m_sName+".txt",*pCurrSequence);
#endif //WRITE_BGSUB_METRICS_ANALYSIS
#if DISPLAY_BGSUB_DEBUG_OUTPUT
        cv::destroyWindow(sDebugDisplayName);
#endif //DISPLAY_BGSUB_DEBUG_OUTPUT
#if ENABLE_DISPLAY_MOUSE_DEBUG
        cv::setMouseCallback(sMouseDebugDisplayName,OnMouseEvent,nullptr);
        pnLatestMouseX = nullptr;
        pnLatestMouseY = nullptr;
        cv::destroyWindow(sMouseDebugDisplayName);
#endif //ENABLE_DISPLAY_MOUSE_DEBUG
    }
#if HAVE_GLSL
    catch(GLException& e) {std::cerr  << "\nTop level caught GLException:\n\t" << e.what() << std::endl << std::endl;}
#endif //HAVE_GLSL
    catch(cv::Exception& e) {std::cerr  << "\nTop level caught cv::Exception:\n\t" << e.what() << std::endl << std::endl;}
    catch(std::runtime_error& e) {std::cerr  << "\nTop level caught std::runtime_error:\n\t" << e.what() << std::endl << std::endl;}
    catch(...) {std::cerr << "\nTop level caught unhandled exception" << std::endl << std::endl;}
#if HAVE_GPU_SUPPORT
#if HAVE_GLSL
    if(bContextInitialized)
        glfwTerminate();
#endif //HAVE_GLSL
#else //!HAVE_GPU_SUPPORT
    g_nActiveThreads--;
#endif //!HAVE_GPU_SUPPORT
    return 0;
}
