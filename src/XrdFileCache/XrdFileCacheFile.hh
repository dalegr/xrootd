#ifndef __XRDFILECACHE_FILE_HH__
#define __XRDFILECACHE_FILE_HH__
//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClDefaultEnv.hh"

#include "XrdFileCacheInfo.hh"
#include "XrdFileCacheStats.hh"

#include <string>
#include <map>


class XrdJob;
class XrdOucIOVec;
namespace XrdCl
{
class Log;
}

namespace XrdFileCache {
   class BlockResponseHandler;
   class DirectResponseHandler;
}



namespace XrdFileCache
{
   class RefCounted
   {
      int m_refcnt;

      RefCounted() : m_refcnt(0) {}
   };

   class File;

   class Block
   {
   public:
      std::vector<char>   m_buff;
      long long           m_offset;
      File               *m_file;
      bool                m_prefetch;
      int                 m_refcnt;
      int                 m_errno;
      bool                m_downloaded;

      Block(File *f, long long off, int size, bool m_prefetch) :
         m_offset(off), m_file(f), m_prefetch(false), m_refcnt(0),
         m_errno(0), m_downloaded(false)
      {
         m_buff.resize(size);
      }

      const char* get_buff(long long pos = 0) const { return &m_buff[pos]; }

      bool is_finished() { return m_downloaded || m_errno != 0; }
      bool is_ok()       { return m_downloaded; }
      bool is_failed()   { return m_errno != 0; }

      void set_error_and_free(int err)
      {
         m_errno = err;
         m_buff.resize(0);
      }
   };

   class File
   {
   private:
      XrdOucCacheIO  &m_input;          //!< original data source
      XrdOssDF       *m_output;         //!< file handle for data file on disk
      XrdOssDF       *m_infoFile;       //!< file handle for data-info file on disk
      Info            m_cfi;            //!< download status of file blocks and access statistics

      std::string     m_temp_filename;  //!< filename of data file on disk
      long long       m_offset;         //!< offset of cached file for block-based operation
      long long       m_fileSize;       //!< size of cached disk file for block-based operation

      bool m_failed; //!< reading from original source or writing to disk has failed
      bool m_stopping; //!< run thread should be stopped

      XrdSysCondVar m_stateCond; //!< state condition variable

      // fsync
      XrdSysMutex m_syncStatusMutex; //!< mutex locking fsync status
      XrdJob *m_syncer;
      std::vector<int> m_writes_during_sync;
      int m_non_flushed_cnt;
      bool m_in_sync;

      typedef std::list<int>         IntList_t;
      typedef IntList_t::iterator    IntList_i;

      typedef std::list<Block*>      BlockList_t;
      typedef BlockList_t::iterator  BlockList_i;

      typedef std::map<int, Block*>  BlockMap_t;
      typedef BlockMap_t::iterator   BlockMap_i;


      BlockMap_t      m_block_map;

      XrdSysCondVar   m_downloadCond;

      Stats           m_stats;      //!< cache statistics, used in IO detach

      int             m_prefetchReadCnt;
      int             m_prefetchHitCnt;

   public:

      //------------------------------------------------------------------------
      //! Constructor.
      //------------------------------------------------------------------------
      File(XrdOucCacheIO &io, std::string &path,
           long long offset, long long fileSize);

      //------------------------------------------------------------------------
      //! Destructor.
      //------------------------------------------------------------------------
      ~File();

      //! Open file handle for data file and info file on local disk.
      bool Open();

      //! Vector read from disk if block is already downloaded, else ReadV from client.
      int ReadV (const XrdOucIOVec *readV, int n);

      int Read(char* buff, long long offset, int size);

      //----------------------------------------------------------------------
      //! \brief Initiate close. Return true if still IO active.
      //! Used in XrdPosixXrootd::Close()
      //----------------------------------------------------------------------
      bool InitiateClose();

      //----------------------------------------------------------------------
      //! Sync file cache inf o and output data with disk
      //----------------------------------------------------------------------
      void Sync();

      //----------------------------------------------------------------------
      //! Reference to prefetch statistics.
      //----------------------------------------------------------------------
      Stats& GetStats() { return m_stats; }


      void ProcessBlockResponse(Block* b, XrdCl::XRootDStatus *status);
      void WriteBlockToDisk(Block* b);

      void Prefetch();

      float GetPrefetchScore();

   private:
      Block* RequestBlock(int i, bool prefetch);

      int    RequestBlocksDirect(DirectResponseHandler *handler, IntList_t& blocks,
                                char* buff, long long req_off, long long req_size);

      int    ReadBlocksFromDisk(IntList_t& blocks,
                                char* req_buf, long long req_off, long long req_size);


       long long BufferSize();

      void CheckPrefetchStatRAM(Block* b);
      void CheckPrefetchStatDisk(int idx);

      //! Short log alias.
      XrdCl::Log* clLog() const { return XrdCl::DefaultEnv::GetLog(); }


      //! Log path
      const char* lPath() const;

      void inc_ref_count(Block*);
      void dec_ref_count(Block*);
   
   };


   // ================================================================

   class BlockResponseHandler : public XrdCl::ResponseHandler
   {
   public:
      Block *m_block;

      BlockResponseHandler(Block *b) : m_block(b) {}

      void HandleResponse(XrdCl::XRootDStatus *status,
                          XrdCl::AnyObject    *response);
   };

   class DirectResponseHandler : public XrdCl::ResponseHandler
   {
   public:
      XrdSysCondVar  m_cond;
      int            m_to_wait;
      int            m_errno;

      DirectResponseHandler(int to_wait) : m_cond(0), m_to_wait(to_wait), m_errno(0) {}

      bool is_finished() { XrdSysCondVarHelper _lck(m_cond); return m_to_wait == 0; }
      bool is_ok()       { XrdSysCondVarHelper _lck(m_cond); return m_to_wait == 0 && m_errno == 0; }
      bool is_failed()   { XrdSysCondVarHelper _lck(m_cond); return m_errno != 0; }

      void HandleResponse(XrdCl::XRootDStatus *status,
                          XrdCl::AnyObject    *response);
   };

}

#endif