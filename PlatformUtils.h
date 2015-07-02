#pragma once

#include <queue>
#include <string>
#include <algorithm>
#include <vector>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <map>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <unordered_map>
#include <deque>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#ifndef XSTR_BASE
#define XSTR_CONCAT(s1,s2) XSTR_CONCAT_BASE(s1,s2)
#define XSTR_CONCAT_BASE(s1,s2) s1##s2
#define XSTR(s) XSTR_BASE(s)
#define XSTR_BASE(s) #s
#endif //XSTR_BASE
#define TIMER_TIC(x) int64 XSTR_CONCAT(__nCPUTimerTick_,x) = cv::getTickCount()
#define TIMER_TOC(x) int64 XSTR_CONCAT(__nCPUTimerVal_,x) = cv::getTickCount()-XSTR_CONCAT(__nCPUTimerTick_,x)
#define TIMER_ELAPSED_MS(x) (double(XSTR_CONCAT(__nCPUTimerVal_,x))/(cv::getTickFrequency()/1000))
#if __cplusplus<201103L
#error "This project requires C++11 support."
#endif //__cplusplus<=201103L
#if (defined WIN32 || defined _WIN32 || defined WIN64 || defined _WIN64)
#define NOMINMAX
#include <windows.h>
#define PLATFORM_USES_WIN32API (WINVER>0x0599)
#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#ifndef DBG_NEW
#define DBG_NEW new (_NORMAL_BLOCK , __FILE__ , __LINE__)
#define new DBG_NEW
#endif //!DBG_NEW
#endif //_DEBUG
#endif //WIN32
#if PLATFORM_USES_WIN32API
#include <stdint.h>
#define __func__ __FUNCTION__
#else //!PLATFORM_USES_WIN32API
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif //!PLATFORM_USES_WIN32API
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <condition_variable>

template<typename Derived,typename Base,typename Del> std::unique_ptr<Derived,Del> static_unique_ptr_cast(std::unique_ptr<Base,Del>&& p) {
    auto d = static_cast<Derived*>(p.release());
    return std::unique_ptr<Derived,Del>(d,std::move(p.get_deleter()));
}

template<typename Derived,typename Base,typename Del> std::unique_ptr<Derived,Del> dynamic_unique_ptr_cast(std::unique_ptr<Base,Del>&& p) {
    if(Derived* result = dynamic_cast<Derived*>(p.get())) {
        p.release();
        return std::unique_ptr<Derived,Del>(result,std::move(p.get_deleter()));
    }
    return std::unique_ptr<Derived,Del>(nullptr,p.get_deleter());
}

namespace PlatformUtils {

    void GetFilesFromDir(const std::string& sDirPath, std::vector<std::string>& vsFilePaths);
    void GetSubDirsFromDir(const std::string& sDirPath, std::vector<std::string>& vsSubDirPaths);
    bool CreateDirIfNotExist(const std::string& sDirPath);

    inline bool compare_lowercase(const std::string& i, const std::string& j) {
        std::string i_lower(i), j_lower(j);
        std::transform(i_lower.begin(),i_lower.end(),i_lower.begin(),tolower);
        std::transform(j_lower.begin(),j_lower.end(),j_lower.begin(),tolower);
        return i_lower<j_lower;
    }

    template<typename T> inline int decimal_integer_digit_count(T number) {
        int digits = number<0?1:0;
        while(std::abs(number)>=1) {
            number /= 10;
            digits++;
        }
        return digits;
    }

#if PLATFORM_USES_WIN32API
    void SetConsoleWindowSize(int x, int y, int buffer_lines=-1);
#endif //PLATFORM_USES_WIN32API

}; //namespace PlatformUtils
