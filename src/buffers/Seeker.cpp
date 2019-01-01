/*
*      Copyright (C) 2017 Mike Burgett
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with XBMC; see the file COPYING.  If not, write to
*  the Free Software Foundation, 51 Franklin Street, Fifth Floor, Boston,
*  MA 02110-1301  USA
*  http://www.gnu.org/copyleft/gpl.html
*
*/

#include "Seeker.h"

using namespace timeshift;
using namespace ADDON;

bool Seeker::InitSeek(int64_t offset, int whence)
{
  int64_t temp;
  if (whence == SEEK_SET)
  {
    temp = offset;
  }
  else if (whence == SEEK_CUR)
  { 
    temp = offset + m_pSd->streamPosition;
  }
  else if (whence == SEEK_END)
  {
    temp = offset + m_pSd->lastKnownLength;
  }
  else
  {
    return false;  // Unrecognized.
  }
  
  // Prevent seeking beyond live point.
  if (temp > m_pSd->lastKnownLength)
    temp = m_pSd->lastKnownLength;
  
  m_iBlockOffset = temp % m_pSd->inputBlockSize;
  m_xStreamOffset = temp - m_iBlockOffset;
  m_bSeeking = true;
  XBMC->Log(LOG_DEBUG, "block: %d, stream: %lli, m_bSeeking: %d", m_iBlockOffset, m_xStreamOffset, m_bSeeking);
  return true;
}

bool Seeker::PreprocessSeek()
{
  XBMC->Log(LOG_DEBUG, "PreprocessSeek()");
  
  bool do_seek = false;  // if true, we have to do seek the non-optimized way.
  int64_t curStreamPtr = m_pSd->streamPosition.load();
  int curOffset = curStreamPtr % m_pSd->inputBlockSize;
  int64_t curBlock = curStreamPtr - curOffset;
  // Moving within the same block (happens at every playback start) 
  if (curBlock == m_xStreamOffset) 
  {  // We're in the same block!
    int moveOffset = m_iBlockOffset - curOffset;
    XBMC->Log(LOG_DEBUG, "%s:%d: curBlock: %lli, curOffset: %d, moveBack: %d", __FUNCTION__, __LINE__, curBlock, curOffset, moveOffset);
    m_pSd->streamPosition.fetch_add(moveOffset);
    m_cirBuf->AdjustBytes(moveOffset);
    m_bSeeking = false;
  }
  else
  {
    if (curBlock < m_xStreamOffset)
    {  // seek forward
      int64_t seekTarget = m_xStreamOffset + m_iBlockOffset;
      if (m_xStreamOffset < m_pSd->lastBlockBuffered)
      { // Seeking forward in buffer.
        int seekDiff = (int )(seekTarget - curStreamPtr);
        m_pSd->streamPosition.store(seekTarget);
        m_cirBuf->AdjustBytes(seekDiff);
      }
      else if (m_xStreamOffset < m_pSd->requestBlock)
      {  // Block not buffered, but has been requested.
        m_bSeekBlockRequested = true;
        m_cirBuf->Reset();
        XBMC->Log(LOG_DEBUG, "%s:%d: currentWindowSize = %d", __FUNCTION__, __LINE__, m_pSd->currentWindowSize);
        m_pSd->currentWindowSize -= (int )((curBlock - m_pSd->lastBlockBuffered) / m_pSd->inputBlockSize);
        m_pSd->currentWindowSize = std::min(0,  m_pSd->currentWindowSize);
        XBMC->Log(LOG_DEBUG, "%s:%d: currentWindowSize = %d", __FUNCTION__, __LINE__, m_pSd->currentWindowSize);
      }
      else
      { // Outside both buffer, and requested range, handle 'normally'
        XBMC->Log(LOG_DEBUG, "%s:%d:", __FUNCTION__, __LINE__);
        do_seek = true;
      }
    }
    else
    {  // Only seek backwards we can optimize was handled already
      XBMC->Log(LOG_DEBUG, "%s:%d:", __FUNCTION__, __LINE__);
      do_seek = true;
    }
  }
  XBMC->Log(LOG_DEBUG, "PreprocessSeek() returning %d", do_seek);
  if (do_seek)
  {
    // 'clear' the circular buffer.
    m_cirBuf->Reset();
    m_pSd->currentWindowSize = 0; // Full request window.
  }
  return do_seek;
}

void Seeker::ProcessRequests()
{
  if (m_bSeeking)
  {
    m_streamPositionSet = false;
    if (!m_bSeekBlockRequested)
    {
      m_pSd->requestBlock = m_xStreamOffset;
      m_pSd->currentWindowSize = 0; // Request all blocks in window
      m_bSeekBlockRequested = true;
    }
  }
}

bool Seeker::PostprocessSeek(int64_t blockNo)
{
  // seeked block has just been buffered!
  // reset seek mechanism
  bool retVal = false;
  if (m_bSeeking) 
  {
    if (blockNo == m_xStreamOffset)
    {
      if (!m_streamPositionSet)
      {
        m_pSd->streamPosition.store(m_xStreamOffset + m_iBlockOffset);
        m_cirBuf->AdjustBytes(m_iBlockOffset);
        m_streamPositionSet = true;
        XBMC->Log(LOG_DEBUG, "%s:%d - m_xStreamOffset: %llu, m_iBlockOffset: %d", __FUNCTION__, __LINE__, m_xStreamOffset, m_iBlockOffset);
      }
      if (m_iBlockOffset)
      {  // Go around one more time.
        XBMC->Log(LOG_DEBUG, "%s:%d", __FUNCTION__, __LINE__);
        m_iBlockOffset = 0;
        m_xStreamOffset += m_pSd->inputBlockSize;
        retVal = false;
      }
      else
      {
        m_bSeekBlockRequested = false;
        m_bSeeking = false;
        m_xStreamOffset = -1;
        retVal = true;
      }
    }
  }
  return retVal;
}
