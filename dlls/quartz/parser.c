/*
 * Implements IBaseFilter for parsers. (internal)
 *
 * hidenori@a2.ctktv.ne.jp
 *
 * FIXME - handle errors/flushing correctly.
 * FIXME - handle seeking.
 */

#include "config.h"

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "mmsystem.h"
#include "winerror.h"
#include "strmif.h"
#include "control.h"
#include "vfwmsgs.h"
#include "uuids.h"

#include "debugtools.h"
DEFAULT_DEBUG_CHANNEL(quartz);

#include "quartz_private.h"
#include "parser.h"
#include "mtype.h"
#include "memalloc.h"

#define	QUARTZ_MSG_FLUSH	(WM_APP+1)
#define	QUARTZ_MSG_EXITTHREAD	(WM_APP+2)
#define	QUARTZ_MSG_SEEK			(WM_APP+3)

HRESULT CParserOutPinImpl_InitIMediaSeeking( CParserOutPinImpl* This );
void CParserOutPinImpl_UninitIMediaSeeking( CParserOutPinImpl* This );
HRESULT CParserOutPinImpl_InitIMediaPosition( CParserOutPinImpl* This );
void CParserOutPinImpl_UninitIMediaPosition( CParserOutPinImpl* This );

/***************************************************************************
 *
 *	CParserImpl internal methods
 *
 */

static
void CParserImpl_SetAsyncReader( CParserImpl* This, IAsyncReader* pReader )
{
	if ( This->m_pReader != NULL )
	{
		IAsyncReader_Release( This->m_pReader );
		This->m_pReader = NULL;
	}
	if ( pReader != NULL )
	{
		This->m_pReader = pReader;
		IAsyncReader_AddRef(This->m_pReader);
	}
}

static
void CParserImpl_ReleaseOutPins( CParserImpl* This )
{
	ULONG nIndex;

	if ( This->m_ppOutPins != NULL )
	{
		for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
		{
			if ( This->m_ppOutPins[nIndex] != NULL )
			{
				IUnknown_Release(This->m_ppOutPins[nIndex]->unk.punkControl);
				This->m_ppOutPins[nIndex] = NULL;
			}
		}
		QUARTZ_FreeMem(This->m_ppOutPins);
		This->m_ppOutPins = NULL;
	}
	This->m_cOutStreams = 0;
}

static
BOOL CParserImpl_OutPinsAreConnected( CParserImpl* This )
{
	QUARTZ_CompListItem*	pItem;
	IPin*	pPin;
	IPin*	pPinPeer;
	HRESULT hr;

	QUARTZ_CompList_Lock( This->basefilter.pOutPins );
	pItem = QUARTZ_CompList_GetFirst( This->basefilter.pOutPins );
	while ( pItem != NULL )
	{
		if ( pItem == NULL )
			break;
		pPin = (IPin*)QUARTZ_CompList_GetItemPtr(pItem);
		pPinPeer = NULL;
		hr = IPin_ConnectedTo(pPin,&pPinPeer);
		if ( hr == S_OK && pPinPeer != NULL )
		{
			IPin_Release(pPinPeer);
			return TRUE;
		}
		pItem = QUARTZ_CompList_GetNext( This->basefilter.pOutPins, pItem );
	}
	QUARTZ_CompList_Unlock( This->basefilter.pOutPins );

	return FALSE;
}

static
void CParserImpl_ReleaseListOfOutPins( CParserImpl* This )
{
	QUARTZ_CompListItem*	pItem;

	QUARTZ_CompList_Lock( This->basefilter.pOutPins );
	while ( 1 )
	{
		pItem = QUARTZ_CompList_GetFirst( This->basefilter.pOutPins );
		if ( pItem == NULL )
			break;
		QUARTZ_CompList_RemoveComp(
			This->basefilter.pOutPins,
			QUARTZ_CompList_GetItemPtr(pItem) );
	}
	QUARTZ_CompList_Unlock( This->basefilter.pOutPins );
}

static
void CParserImpl_ClearAllRequests( CParserImpl* This )
{
	ULONG	nIndex;

	TRACE("(%p)\n",This);

	for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
	{
		This->m_ppOutPins[nIndex]->m_bReqUsed = FALSE;
		This->m_ppOutPins[nIndex]->m_pReqSample = NULL;
	}
}

static
void CParserImpl_ReleaseAllRequests( CParserImpl* This )
{
	ULONG	nIndex;

	TRACE("(%p)\n",This);

	for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
	{
		if ( This->m_ppOutPins[nIndex]->m_bReqUsed )
		{
			if ( This->m_ppOutPins[nIndex]->m_pReqSample != NULL )
			{
				IMediaSample_Release(This->m_ppOutPins[nIndex]->m_pReqSample);
				This->m_ppOutPins[nIndex]->m_pReqSample = NULL;
			}
			This->m_ppOutPins[nIndex]->m_bReqUsed = FALSE;
		}
	}
}

static
BOOL CParserImpl_HasPendingSamples( CParserImpl* This )
{
	ULONG	nIndex;

	for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
	{
		if ( This->m_ppOutPins[nIndex]->m_bReqUsed &&
			 This->m_ppOutPins[nIndex]->m_pReqSample != NULL )
			return TRUE;
	}

	return FALSE;
}

static
HRESULT CParserImpl_FlushAllPendingSamples( CParserImpl* This )
{
	HRESULT hr;
	IMediaSample*	pSample;
	DWORD_PTR	dwContext;

	TRACE("(%p)\n",This);

	/* remove all samples from queue */
	hr = IAsyncReader_BeginFlush(This->m_pReader);
	if ( FAILED(hr) )
		return hr;
	IAsyncReader_EndFlush(This->m_pReader);

	while ( 1 )
	{
		hr = IAsyncReader_WaitForNext(This->m_pReader,0,&pSample,&dwContext);
		if ( hr != S_OK )
			break;
	}
	CParserImpl_ReleaseAllRequests(This);

	return NOERROR;
}

static HRESULT CParserImpl_SendEndOfStream( CParserImpl* This )
{
	ULONG	nIndex;
	HRESULT hr;
	HRESULT hrRet;
	CParserOutPinImpl*	pOutPin;

	TRACE("(%p)\n",This);
	if ( This->m_bSendEOS )
		return NOERROR;
	This->m_bSendEOS = TRUE;

	hrRet = S_OK;
	for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
	{
		pOutPin = This->m_ppOutPins[nIndex];
		hr = CPinBaseImpl_SendEndOfStream(&pOutPin->pin);
		if ( FAILED(hr) )
		{
			if ( SUCCEEDED(hrRet) )
				hrRet = hr;
		}
		else
		{
			if ( hr != S_OK && hrRet == S_OK )
				hrRet = hr;
		}
	}

	return hrRet;
}

static HRESULT CParserImpl_SendFlush( CParserImpl* This )
{
	ULONG	nIndex;
	HRESULT hr;
	HRESULT hrRet;
	CParserOutPinImpl*	pOutPin;

	TRACE("(%p)\n",This);
	hrRet = S_OK;
	for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
	{
		pOutPin = This->m_ppOutPins[nIndex];
		hr = CPinBaseImpl_SendBeginFlush(&pOutPin->pin);
		if ( FAILED(hr) )
		{
			if ( SUCCEEDED(hrRet) )
				hrRet = hr;
		}
		else
		{
			if ( hr != S_OK && hrRet == S_OK )
				hrRet = hr;
			hr = CPinBaseImpl_SendEndFlush(&pOutPin->pin);
			if ( FAILED(hr) )
				hrRet = hr;
		}
	}

	return hrRet;
}

static
HRESULT CParserImpl_ProcessNextSample( CParserImpl* This )
{
	IMediaSample*	pSample;
	DWORD_PTR	dwContext;
	ULONG	nIndex;
	HRESULT hr;
	CParserOutPinImpl*	pOutPin;
	MSG	msg;

	while ( 1 )
	{
		if ( PeekMessageA( &msg, (HWND)NULL, 0, 0, PM_REMOVE ) )
		{
			hr = NOERROR;
			switch ( msg.message )
			{
			case QUARTZ_MSG_FLUSH:
				TRACE("Flush\n");
				CParserImpl_FlushAllPendingSamples(This);
				hr = CParserImpl_SendFlush(This);
				break;
			case QUARTZ_MSG_EXITTHREAD:
				TRACE("(%p) EndThread\n",This);
				CParserImpl_FlushAllPendingSamples(This);
				CParserImpl_ClearAllRequests(This);
				CParserImpl_SendFlush(This);
				CParserImpl_SendEndOfStream(This);
				TRACE("(%p) exit thread\n",This);
				return S_FALSE;
			case QUARTZ_MSG_SEEK:
				FIXME("Seek\n");
				CParserImpl_FlushAllPendingSamples(This);
				break;
			default:
				FIXME( "invalid message %04u\n", (unsigned)msg.message );
				/* Notify (ABORT) */
				hr = E_FAIL;
			}

			return hr;
		}

		hr = IAsyncReader_WaitForNext(This->m_pReader,PARSER_POLL_INTERVAL,&pSample,&dwContext);
		nIndex = (ULONG)dwContext;
		if ( hr != VFW_E_TIMEOUT )
			break;
	}
	if ( FAILED(hr) )
	{
		return hr;
	}

	pOutPin = This->m_ppOutPins[nIndex];
	if ( pOutPin != NULL && pOutPin->m_bReqUsed )
	{
		if ( This->m_pHandler->pProcessSample != NULL )
			hr = This->m_pHandler->pProcessSample(This,nIndex,pOutPin->m_llReqStart,pOutPin->m_lReqLength,pOutPin->m_pReqSample);

		if ( SUCCEEDED(hr) )
		{
			if ( pOutPin->m_pOutPinAllocator != NULL &&
				 pOutPin->m_pOutPinAllocator != This->m_pAllocator )
			{
				/* if pin has its own allocator, sample must be copied */
				hr = IMemAllocator_GetBuffer( This->m_pAllocator, &pSample, NULL, NULL, 0 );
				if ( SUCCEEDED(hr) )
				{
					hr = QUARTZ_IMediaSample_Copy(
						pSample, pOutPin->m_pReqSample, TRUE );
					if ( SUCCEEDED(hr) )
						hr = CPinBaseImpl_SendSample(&pOutPin->pin,pSample);
					IMediaSample_Release(pSample);
				}
			}
			else
			{
				hr = CPinBaseImpl_SendSample(&pOutPin->pin,pOutPin->m_pReqSample);
			}
		}

		if ( FAILED(hr) )
		{
			/* Notify (ABORT) */
		}

		IMediaSample_Release(pOutPin->m_pReqSample);
		pOutPin->m_pReqSample = NULL;
		pOutPin->m_bReqUsed = FALSE;
	}

	if ( SUCCEEDED(hr) )
		hr = NOERROR;

	TRACE("return %08lx\n",hr);

	return hr;
}

static
DWORD WINAPI CParserImpl_ThreadEntry( LPVOID pv )
{
	CParserImpl*	This = (CParserImpl*)pv;
	BOOL	bReqNext;
	ULONG	nIndex = 0;
	HRESULT hr;
	REFERENCE_TIME	rtSampleTimeStart, rtSampleTimeEnd;
	LONGLONG	llReqStart;
	LONG	lReqLength;
	REFERENCE_TIME	rtReqStart, rtReqStop;
	IMediaSample*	pSample;
	MSG	msg;

	/* initialize the message queue. */
	PeekMessageA( &msg, (HWND)NULL, 0, 0, PM_NOREMOVE );

	CParserImpl_ClearAllRequests(This);

	/* resume the owner thread. */
	SetEvent( This->m_hEventInit );

	TRACE( "Enter message loop.\n" );

	bReqNext = TRUE;
	while ( 1 )
	{
		if ( bReqNext )
		{
			/* Get the next request.  */
			hr = This->m_pHandler->pGetNextRequest( This, &nIndex, &llReqStart, &lReqLength, &rtReqStart, &rtReqStop );
			if ( FAILED(hr) )
			{
				/* Notify (ABORT) */
				break;
			}
			if ( hr != S_OK )
			{
				/* Flush pending samples. */
				hr = S_OK;
				while ( CParserImpl_HasPendingSamples(This) )
				{
					hr = CParserImpl_ProcessNextSample(This);
					if ( hr != S_OK )
						break;
				}
				if ( hr != S_OK )
				{
					/* notification is already sent */
					break;
				}

				/* Send End Of Stream. */
				hr = CParserImpl_SendEndOfStream(This);
				if ( hr != S_OK )
				{
					/* notification is already sent */
					break;
				}

				/* Blocking... */
				hr = CParserImpl_ProcessNextSample(This);
				if ( hr != S_OK )
				{
					/* notification is already sent */
					break;
				}
				continue;
			}
			if ( This->m_ppOutPins[nIndex]->pin.pPinConnectedTo == NULL )
				continue;

			rtSampleTimeStart = This->basefilter.rtStart + llReqStart * QUARTZ_TIMEUNITS;
			rtSampleTimeEnd = (llReqStart + lReqLength) * QUARTZ_TIMEUNITS;
			bReqNext = FALSE;
		}

		if ( !This->m_ppOutPins[nIndex]->m_bReqUsed )
		{
			hr = IMemAllocator_GetBuffer( This->m_pAllocator, &pSample, NULL, NULL, 0 );
			if ( FAILED(hr) )
			{
				/* Notify (ABORT) */
				break;
			}
			hr = IMediaSample_SetTime(pSample,&rtSampleTimeStart,&rtSampleTimeEnd);
			if ( SUCCEEDED(hr) )
				hr = IAsyncReader_Request(This->m_pReader,pSample,nIndex);
			if ( FAILED(hr) )
			{
				/* Notify (ABORT) */
				break;
			}

			This->m_ppOutPins[nIndex]->m_bReqUsed = TRUE;
			This->m_ppOutPins[nIndex]->m_pReqSample = pSample;
			This->m_ppOutPins[nIndex]->m_llReqStart = llReqStart;
			This->m_ppOutPins[nIndex]->m_lReqLength = lReqLength;
			This->m_ppOutPins[nIndex]->m_rtReqStart = rtSampleTimeStart;
			This->m_ppOutPins[nIndex]->m_rtReqStop = rtSampleTimeEnd;
			bReqNext = TRUE;
			continue;
		}

		hr = CParserImpl_ProcessNextSample(This);
		if ( hr != S_OK )
		{
			/* notification is already sent */
			break;
		}
	}

	return 0;
}

static
HRESULT CParserImpl_BeginThread( CParserImpl* This )
{
	DWORD dwRes;
	HANDLE hEvents[2];

	if ( This->m_hEventInit != (HANDLE)NULL &&
		 This->m_hThread != (HANDLE)NULL )
		return NOERROR;

	This->m_hEventInit = CreateEventA(NULL,TRUE,FALSE,NULL);
	if ( This->m_hEventInit == (HANDLE)NULL )
		return E_OUTOFMEMORY;

	/* create the processing thread. */
	This->m_hThread = CreateThread(
		NULL, 0,
		CParserImpl_ThreadEntry,
		(LPVOID)This,
		0, &This->m_dwThreadId );
	if ( This->m_hThread == (HANDLE)NULL )
		return E_FAIL;

	hEvents[0] = This->m_hEventInit;
	hEvents[1] = This->m_hThread;

	dwRes = WaitForMultipleObjects(2,hEvents,FALSE,INFINITE);
	if ( dwRes != WAIT_OBJECT_0 )
		return E_FAIL;

	return NOERROR;
}

static
void CParserImpl_EndThread( CParserImpl* This )
{
	TRACE("(%p)\n",This);
	if ( This->m_hThread != (HANDLE)NULL )
	{
		if ( PostThreadMessageA(
			This->m_dwThreadId, QUARTZ_MSG_EXITTHREAD, 0, 0 ) )
		{
			WaitForSingleObject( This->m_hThread, INFINITE );
		}
		CloseHandle( This->m_hThread );
		This->m_hThread = (HANDLE)NULL;
		This->m_dwThreadId = 0;
	}
	if ( This->m_hEventInit != (HANDLE)NULL )
	{
		CloseHandle( This->m_hEventInit );
		This->m_hEventInit = (HANDLE)NULL;
	}
}

static
HRESULT CParserImpl_MemCommit( CParserImpl* This )
{
	HRESULT hr;
	ULONG	nIndex;
	IMemAllocator*	pAlloc;

	TRACE("(%p)\n",This);

	if ( This->m_pAllocator == NULL )
		return E_UNEXPECTED;

	hr = IMemAllocator_Commit( This->m_pAllocator );
	if ( FAILED(hr) )
		return hr;

	if ( This->m_ppOutPins != NULL && This->m_cOutStreams > 0 )
	{
		for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
		{
			pAlloc = This->m_ppOutPins[nIndex]->m_pOutPinAllocator;
			if ( pAlloc != NULL && pAlloc != This->m_pAllocator )
			{
				hr = IMemAllocator_Commit( pAlloc );
				if ( FAILED(hr) )
					return hr;
			}
		}
	}

	return NOERROR;
}

static
void CParserImpl_MemDecommit( CParserImpl* This )
{
	ULONG	nIndex;
	IMemAllocator*	pAlloc;

	TRACE("(%p)\n",This);

	if ( This->m_pAllocator != NULL )
		IMemAllocator_Decommit( This->m_pAllocator );

	if ( This->m_ppOutPins != NULL && This->m_cOutStreams > 0 )
	{
		for ( nIndex = 0; nIndex < This->m_cOutStreams; nIndex++ )
		{
			pAlloc = This->m_ppOutPins[nIndex]->m_pOutPinAllocator;
			if ( pAlloc != NULL )
				IMemAllocator_Decommit( pAlloc );
		}
	}
}



/***************************************************************************
 *
 *	CParserImpl methods
 *
 */

static HRESULT CParserImpl_OnActive( CBaseFilterImpl* pImpl )
{
	CParserImpl_THIS(pImpl,basefilter);

	TRACE( "(%p)\n", This );

	if ( !CParserImpl_OutPinsAreConnected(This) )
		return NOERROR;

	return NOERROR;
}

static HRESULT CParserImpl_OnInactive( CBaseFilterImpl* pImpl )
{
	CParserImpl_THIS(pImpl,basefilter);
	HRESULT hr;

	TRACE( "(%p)\n", This );

	if ( !CParserImpl_OutPinsAreConnected(This) )
		return NOERROR;

	hr = CParserImpl_MemCommit(This);
	if ( FAILED(hr) )
		return hr;
	hr = CParserImpl_BeginThread(This);
	if ( FAILED(hr) )
	{
		CParserImpl_EndThread(This);
		return hr;
	}

	return NOERROR;
}

static HRESULT CParserImpl_OnStop( CBaseFilterImpl* pImpl )
{
	CParserImpl_THIS(pImpl,basefilter);

	FIXME( "(%p)\n", This );

	CParserImpl_EndThread(This);
	CParserImpl_MemDecommit(This);
	This->m_bSendEOS = FALSE;

	/* FIXME - reset streams. */

	return NOERROR;
}


static const CBaseFilterHandlers filterhandlers =
{
	CParserImpl_OnActive, /* pOnActive */
	CParserImpl_OnInactive, /* pOnInactive */
	CParserImpl_OnStop, /* pOnStop */
};


/***************************************************************************
 *
 *	CParserInPinImpl methods
 *
 */

static HRESULT CParserInPinImpl_OnPreConnect( CPinBaseImpl* pImpl, IPin* pPin )
{
	CParserInPinImpl_THIS(pImpl,pin);
	HRESULT hr;
	ULONG	nIndex;
	IUnknown*	punk;
	IAsyncReader* pReader = NULL;
	LPCWSTR pwszOutPinName;
	IMemAllocator*	pAllocActual;
	AM_MEDIA_TYPE*	pmt;

	TRACE("(%p,%p)\n",This,pPin);

	if ( This->pParser->m_pHandler->pInitParser == NULL ||
		 This->pParser->m_pHandler->pUninitParser == NULL ||
		 This->pParser->m_pHandler->pGetOutPinName == NULL ||
		 This->pParser->m_pHandler->pGetStreamType == NULL ||
		 This->pParser->m_pHandler->pCheckStreamType == NULL ||
		 This->pParser->m_pHandler->pGetAllocProp == NULL ||
		 This->pParser->m_pHandler->pGetNextRequest == NULL )
	{
		FIXME("this parser is not implemented.\n");
		return E_NOTIMPL;
	}

	/* at first, release all output pins. */
	if ( CParserImpl_OutPinsAreConnected(This->pParser) )
		return E_FAIL;
	CParserImpl_ReleaseListOfOutPins(This->pParser);
	CParserImpl_ReleaseOutPins(This->pParser);

	CParserImpl_SetAsyncReader( This->pParser, NULL );
	hr = IPin_QueryInterface( pPin, &IID_IAsyncReader, (void**)&pReader );
	if ( FAILED(hr) )
		return hr;
	CParserImpl_SetAsyncReader( This->pParser, pReader );
	IAsyncReader_Release(pReader);

	/* initialize parser. */
	hr = This->pParser->m_pHandler->pInitParser(This->pParser,&This->pParser->m_cOutStreams);
	if ( FAILED(hr) )
		return hr;
	This->pParser->m_ppOutPins = (CParserOutPinImpl**)QUARTZ_AllocMem(
		sizeof(CParserOutPinImpl*) * This->pParser->m_cOutStreams );
	if ( This->pParser->m_ppOutPins == NULL )
		return E_OUTOFMEMORY;
	for ( nIndex = 0; nIndex < This->pParser->m_cOutStreams; nIndex++ )
		This->pParser->m_ppOutPins[nIndex] = NULL;

	/* create and initialize an allocator. */
	hr = This->pParser->m_pHandler->pGetAllocProp(This->pParser,&This->pParser->m_propAlloc);
	if ( FAILED(hr) )
		return hr;
	if ( This->pParser->m_propAlloc.cbAlign == 0 )
		This->pParser->m_propAlloc.cbAlign = 1;

	if ( This->pParser->m_pAllocator == NULL )
	{
		hr = QUARTZ_CreateMemoryAllocator(NULL,(void**)&punk);
		if ( FAILED(hr) )
			return hr;
		hr = IUnknown_QueryInterface( punk, &IID_IMemAllocator, (void**)&This->pParser->m_pAllocator );
		IUnknown_Release(punk);
		if ( FAILED(hr) )
			return hr;
	}
	pAllocActual = NULL;
	hr = IAsyncReader_RequestAllocator(pReader,This->pParser->m_pAllocator,&This->pParser->m_propAlloc,&pAllocActual);
	if ( FAILED(hr) )
		return hr;
	IMemAllocator_Release(This->pParser->m_pAllocator);
	This->pParser->m_pAllocator = pAllocActual;

	/* create output pins. */
	for ( nIndex = 0; nIndex < This->pParser->m_cOutStreams; nIndex++ )
	{
		pwszOutPinName = This->pParser->m_pHandler->pGetOutPinName(This->pParser,nIndex);
		if ( pwszOutPinName == NULL )
			return E_FAIL;
		hr = QUARTZ_CreateParserOutPin(
			This->pParser,
			&This->pParser->m_csParser,
			&This->pParser->m_ppOutPins[nIndex],
			nIndex, pwszOutPinName );
		if ( SUCCEEDED(hr) )
			hr = QUARTZ_CompList_AddTailComp(
				This->pParser->basefilter.pOutPins,
				(IUnknown*)&(This->pParser->m_ppOutPins[nIndex]->pin),
				NULL, 0 );
		if ( FAILED(hr) )
			return hr;
		pmt = &This->pParser->m_ppOutPins[nIndex]->m_mtOut;
		QUARTZ_MediaType_Free( pmt );
		ZeroMemory( pmt, sizeof(AM_MEDIA_TYPE) );
		hr = This->pParser->m_pHandler->pGetStreamType(This->pParser,nIndex,pmt);
		if ( FAILED(hr) )
		{
			ZeroMemory( pmt, sizeof(AM_MEDIA_TYPE) );
			return hr;
		}
		This->pParser->m_ppOutPins[nIndex]->pin.cAcceptTypes = 1;
		This->pParser->m_ppOutPins[nIndex]->pin.pmtAcceptTypes = pmt;
	}

	return NOERROR;
}

static HRESULT CParserInPinImpl_OnDisconnect( CPinBaseImpl* pImpl )
{
	CParserInPinImpl_THIS(pImpl,pin);

	CParserImpl_OnInactive(&This->pParser->basefilter);
	CParserImpl_OnStop(&This->pParser->basefilter);
	if ( This->pParser->m_pHandler->pUninitParser != NULL )
		This->pParser->m_pHandler->pUninitParser(This->pParser);
	CParserImpl_SetAsyncReader( This->pParser, NULL );
	if ( This->pParser->m_pAllocator != NULL )
	{
		IMemAllocator_Decommit(This->pParser->m_pAllocator);
		IMemAllocator_Release(This->pParser->m_pAllocator);
		This->pParser->m_pAllocator = NULL;
	}

	return NOERROR;
}

static HRESULT CParserInPinImpl_CheckMediaType( CPinBaseImpl* pImpl, const AM_MEDIA_TYPE* pmt )
{
	CParserInPinImpl_THIS(pImpl,pin);

	TRACE("(%p,%p)\n",This,pmt);

	if ( !IsEqualGUID( &pmt->majortype, &MEDIATYPE_Stream ) )
		return E_FAIL;

	return NOERROR;
}

static const CBasePinHandlers inputpinhandlers =
{
	CParserInPinImpl_OnPreConnect, /* pOnPreConnect */
	NULL, /* pOnPostConnect */
	CParserInPinImpl_OnDisconnect, /* pOnDisconnect */
	CParserInPinImpl_CheckMediaType, /* pCheckMediaType */
	NULL, /* pQualityNotify */
	NULL, /* pReceive */
	NULL, /* pReceiveCanBlock */
	NULL, /* pEndOfStream */
	NULL, /* pBeginFlush */
	NULL, /* pEndFlush */
	NULL, /* pNewSegment */
};

/***************************************************************************
 *
 *	CParserOutPinImpl methods
 *
 */

static HRESULT CParserOutPinImpl_OnPostConnect( CPinBaseImpl* pImpl, IPin* pPin )
{
	CParserOutPinImpl_THIS(pImpl,pin);
	ALLOCATOR_PROPERTIES	propReq;
	ALLOCATOR_PROPERTIES	propActual;
	IMemAllocator*	pAllocator;
	HRESULT hr;
	BOOL	bNewAllocator = FALSE;

	TRACE("(%p,%p)\n",This,pPin);

	if ( This->pin.pMemInputPinConnectedTo == NULL )
		return E_UNEXPECTED;

	if ( This->m_pOutPinAllocator != NULL )
	{
		IMemAllocator_Release(This->m_pOutPinAllocator);
		This->m_pOutPinAllocator = NULL;
	}

	/* try to use This->pParser->m_pAllocator. */
	ZeroMemory( &propReq, sizeof(ALLOCATOR_PROPERTIES) );
	hr = IMemInputPin_GetAllocatorRequirements(
		This->pin.pMemInputPinConnectedTo, &propReq );
	if ( propReq.cbAlign != 0 )
	{
		if ( This->pParser->m_propAlloc.cbAlign != ( This->pParser->m_propAlloc.cbAlign / propReq.cbAlign * propReq.cbAlign ) )
			bNewAllocator = TRUE;
	}
	if ( propReq.cbPrefix != 0 )
		bNewAllocator = TRUE;
	if ( !bNewAllocator )
	{
		hr = IMemInputPin_NotifyAllocator(
			This->pin.pMemInputPinConnectedTo,
			This->pParser->m_pAllocator, FALSE );
		if ( hr == NOERROR )
		{
			This->m_pOutPinAllocator = This->pParser->m_pAllocator;
			IMemAllocator_AddRef(This->m_pOutPinAllocator);
			return NOERROR;
		}
	}

	hr = IMemInputPin_GetAllocator(
			This->pin.pMemInputPinConnectedTo, &pAllocator );
	if ( FAILED(hr) )
		return hr;
	hr = IMemAllocator_SetProperties( pAllocator, &This->pParser->m_propAlloc, &propActual );
	if ( SUCCEEDED(hr) )
		hr = IMemInputPin_NotifyAllocator(
			This->pin.pMemInputPinConnectedTo, pAllocator, FALSE );
	if ( FAILED(hr) )
	{
		IMemAllocator_Release(pAllocator);
		return hr;
	}

	This->m_pOutPinAllocator = pAllocator;
	return NOERROR;
}

static HRESULT CParserOutPinImpl_OnDisconnect( CPinBaseImpl* pImpl )
{
	CParserOutPinImpl_THIS(pImpl,pin);

	if ( This->m_pOutPinAllocator != NULL )
	{
		IMemAllocator_Release(This->m_pOutPinAllocator);
		This->m_pOutPinAllocator = NULL;
	}

	return NOERROR;
}

static HRESULT CParserOutPinImpl_CheckMediaType( CPinBaseImpl* pImpl, const AM_MEDIA_TYPE* pmt )
{
	CParserOutPinImpl_THIS(pImpl,pin);
	HRESULT hr;

	TRACE("(%p,%p)\n",This,pmt);
	if ( pmt == NULL )
		return E_POINTER;

	if ( This->pParser->m_pHandler->pCheckStreamType == NULL )
		return E_NOTIMPL;

	hr = This->pParser->m_pHandler->pCheckStreamType( This->pParser, This->nStreamIndex, pmt );
	if ( FAILED(hr) )
		return hr;

	return NOERROR;
}


static const CBasePinHandlers outputpinhandlers =
{
	NULL, /* pOnPreConnect */
	CParserOutPinImpl_OnPostConnect, /* pOnPostConnect */
	CParserOutPinImpl_OnDisconnect, /* pOnDisconnect */
	CParserOutPinImpl_CheckMediaType, /* pCheckMediaType */
	NULL, /* pQualityNotify */
	OutputPinSync_Receive, /* pReceive */
	OutputPinSync_ReceiveCanBlock, /* pReceiveCanBlock */
	OutputPinSync_EndOfStream, /* pEndOfStream */
	OutputPinSync_BeginFlush, /* pBeginFlush */
	OutputPinSync_EndFlush, /* pEndFlush */
	OutputPinSync_NewSegment, /* pNewSegment */
};

/***************************************************************************
 *
 *	new/delete CParserImpl
 *
 */

/* can I use offsetof safely? - FIXME? */
static QUARTZ_IFEntry FilterIFEntries[] =
{
  { &IID_IPersist, offsetof(CParserImpl,basefilter)-offsetof(CParserImpl,unk) },
  { &IID_IMediaFilter, offsetof(CParserImpl,basefilter)-offsetof(CParserImpl,unk) },
  { &IID_IBaseFilter, offsetof(CParserImpl,basefilter)-offsetof(CParserImpl,unk) },
};

static void QUARTZ_DestroyParser(IUnknown* punk)
{
	CParserImpl_THIS(punk,unk);

	TRACE( "(%p)\n", This );

	if ( This->m_pInPin != NULL )
		CParserInPinImpl_OnDisconnect(&This->m_pInPin->pin);

	CParserImpl_SetAsyncReader( This, NULL );
	if ( This->m_pAllocator != NULL )
	{
		IMemAllocator_Release(This->m_pAllocator);
		This->m_pAllocator = NULL;
	}
	if ( This->m_pInPin != NULL )
	{
		IUnknown_Release(This->m_pInPin->unk.punkControl);
		This->m_pInPin = NULL;
	}
	CParserImpl_ReleaseOutPins( This );

	DeleteCriticalSection( &This->m_csParser );

	CBaseFilterImpl_UninitIBaseFilter(&This->basefilter);
}

HRESULT QUARTZ_CreateParser(
	IUnknown* punkOuter,void** ppobj,
	const CLSID* pclsidParser,
	LPCWSTR pwszParserName,
	LPCWSTR pwszInPinName,
	const ParserHandlers* pHandler )
{
	CParserImpl*	This = NULL;
	HRESULT hr;

	TRACE("(%p,%p)\n",punkOuter,ppobj);

	This = (CParserImpl*)
		QUARTZ_AllocObj( sizeof(CParserImpl) );
	if ( This == NULL )
		return E_OUTOFMEMORY;
	ZeroMemory( This, sizeof(CParserImpl) );

	This->m_pInPin = NULL;
	This->m_cOutStreams = 0;
	This->m_ppOutPins = NULL;
	This->m_pReader = NULL;
	This->m_pAllocator = NULL;
	ZeroMemory( &This->m_propAlloc, sizeof(ALLOCATOR_PROPERTIES) );
	This->m_hEventInit = (HANDLE)NULL;
	This->m_hThread = (HANDLE)NULL;
	This->m_dwThreadId = 0;
	This->m_bSendEOS = FALSE;
	This->m_pHandler = pHandler;
	This->m_pUserData = NULL;

	QUARTZ_IUnkInit( &This->unk, punkOuter );

	hr = CBaseFilterImpl_InitIBaseFilter(
		&This->basefilter,
		This->unk.punkControl,
		pclsidParser,
		pwszParserName,
		&filterhandlers );
	if ( SUCCEEDED(hr) )
	{
		/* construct this class. */
		hr = S_OK;

		if ( FAILED(hr) )
		{
			CBaseFilterImpl_UninitIBaseFilter(&This->basefilter);
		}
	}

	if ( FAILED(hr) )
	{
		QUARTZ_FreeObj(This);
		return hr;
	}

	This->unk.pEntries = FilterIFEntries;
	This->unk.dwEntries = sizeof(FilterIFEntries)/sizeof(FilterIFEntries[0]);
	This->unk.pOnFinalRelease = QUARTZ_DestroyParser;
	InitializeCriticalSection( &This->m_csParser );

	/* create the input pin. */
	hr = QUARTZ_CreateParserInPin(
		This,
		&This->m_csParser,
		&This->m_pInPin,
		pwszInPinName );
	if ( SUCCEEDED(hr) )
		hr = QUARTZ_CompList_AddComp(
			This->basefilter.pInPins,
			(IUnknown*)&(This->m_pInPin->pin),
			NULL, 0 );

	if ( FAILED(hr) )
	{
		IUnknown_Release( This->unk.punkControl );
		return hr;
	}

	*ppobj = (void*)&(This->unk);

	return S_OK;
}

/***************************************************************************
 *
 *	new/delete CParserInPinImpl
 *
 */

/* can I use offsetof safely? - FIXME? */
static QUARTZ_IFEntry InPinIFEntries[] =
{
  { &IID_IPin, offsetof(CParserInPinImpl,pin)-offsetof(CParserInPinImpl,unk) },
  { &IID_IMemInputPin, offsetof(CParserInPinImpl,meminput)-offsetof(CParserInPinImpl,unk) },
};

static void QUARTZ_DestroyParserInPin(IUnknown* punk)
{
	CParserInPinImpl_THIS(punk,unk);

	TRACE( "(%p)\n", This );

	CPinBaseImpl_UninitIPin( &This->pin );
	CMemInputPinBaseImpl_UninitIMemInputPin( &This->meminput );
}

HRESULT QUARTZ_CreateParserInPin(
	CParserImpl* pFilter,
	CRITICAL_SECTION* pcsPin,
	CParserInPinImpl** ppPin,
	LPCWSTR pwszPinName )
{
	CParserInPinImpl*	This = NULL;
	HRESULT hr;

	TRACE("(%p,%p,%p)\n",pFilter,pcsPin,ppPin);

	This = (CParserInPinImpl*)
		QUARTZ_AllocObj( sizeof(CParserInPinImpl) );
	if ( This == NULL )
		return E_OUTOFMEMORY;

	QUARTZ_IUnkInit( &This->unk, NULL );
	This->pParser = pFilter;

	hr = CPinBaseImpl_InitIPin(
		&This->pin,
		This->unk.punkControl,
		pcsPin,
		&pFilter->basefilter,
		pwszPinName,
		FALSE,
		&inputpinhandlers );

	if ( SUCCEEDED(hr) )
	{
		hr = CMemInputPinBaseImpl_InitIMemInputPin(
			&This->meminput,
			This->unk.punkControl,
			&This->pin );
		if ( FAILED(hr) )
		{
			CPinBaseImpl_UninitIPin( &This->pin );
		}
	}

	if ( FAILED(hr) )
	{
		QUARTZ_FreeObj(This);
		return hr;
	}

	This->unk.pEntries = InPinIFEntries;
	This->unk.dwEntries = sizeof(InPinIFEntries)/sizeof(InPinIFEntries[0]);
	This->unk.pOnFinalRelease = QUARTZ_DestroyParserInPin;

	*ppPin = This;

	TRACE("returned successfully.\n");

	return S_OK;
}


/***************************************************************************
 *
 *	new/delete CParserOutPinImpl
 *
 */

/* can I use offsetof safely? - FIXME? */
static QUARTZ_IFEntry OutPinIFEntries[] =
{
  { &IID_IPin, offsetof(CParserOutPinImpl,pin)-offsetof(CParserOutPinImpl,unk) },
  { &IID_IQualityControl, offsetof(CParserOutPinImpl,qcontrol)-offsetof(CParserOutPinImpl,unk) },
  { &IID_IMediaSeeking, offsetof(CParserOutPinImpl,mediaseeking)-offsetof(CParserOutPinImpl,unk) },
  { &IID_IMediaPosition, offsetof(CParserOutPinImpl,mediaposition)-offsetof(CParserOutPinImpl,unk) },
};

static void QUARTZ_DestroyParserOutPin(IUnknown* punk)
{
	CParserOutPinImpl_THIS(punk,unk);

	TRACE( "(%p)\n", This );

	QUARTZ_MediaType_Free( &This->m_mtOut );
	if ( This->m_pOutPinAllocator != NULL )
		IMemAllocator_Release(This->m_pOutPinAllocator);

	CParserOutPinImpl_UninitIMediaPosition(This);
	CParserOutPinImpl_UninitIMediaSeeking(This);
	CQualityControlPassThruImpl_UninitIQualityControl( &This->qcontrol );
	CPinBaseImpl_UninitIPin( &This->pin );

}

HRESULT QUARTZ_CreateParserOutPin(
	CParserImpl* pFilter,
	CRITICAL_SECTION* pcsPin,
	CParserOutPinImpl** ppPin,
	ULONG nStreamIndex,
	LPCWSTR pwszPinName )
{
	CParserOutPinImpl*	This = NULL;
	HRESULT hr;

	TRACE("(%p,%p,%p)\n",pFilter,pcsPin,ppPin);

	This = (CParserOutPinImpl*)
		QUARTZ_AllocObj( sizeof(CParserOutPinImpl) );
	if ( This == NULL )
		return E_OUTOFMEMORY;

	QUARTZ_IUnkInit( &This->unk, NULL );
	This->pParser = pFilter;
	This->nStreamIndex = nStreamIndex;
	ZeroMemory( &This->m_mtOut, sizeof(AM_MEDIA_TYPE) );
	This->m_pOutPinAllocator = NULL;
	This->m_pUserData = NULL;
	This->m_bReqUsed = FALSE;
	This->m_pReqSample = NULL;
	This->m_llReqStart = 0;
	This->m_lReqLength = 0;
	This->m_rtReqStart = 0;
	This->m_rtReqStop = 0;


	hr = CPinBaseImpl_InitIPin(
		&This->pin,
		This->unk.punkControl,
		pcsPin,
		&pFilter->basefilter,
		pwszPinName,
		TRUE,
		&outputpinhandlers );

	if ( SUCCEEDED(hr) )
	{
		hr = CQualityControlPassThruImpl_InitIQualityControl(
			&This->qcontrol,
			This->unk.punkControl,
			&This->pin );
		if ( SUCCEEDED(hr) )
		{
			hr = CParserOutPinImpl_InitIMediaSeeking(This);
			if ( SUCCEEDED(hr) )
			{
				hr = CParserOutPinImpl_InitIMediaPosition(This);
				if ( FAILED(hr) )
				{
					CParserOutPinImpl_UninitIMediaSeeking(This);
				}
			}
			if ( FAILED(hr) )
			{
				CQualityControlPassThruImpl_UninitIQualityControl( &This->qcontrol );
			}
		}
		if ( FAILED(hr) )
		{
			CPinBaseImpl_UninitIPin( &This->pin );
		}
	}

	if ( FAILED(hr) )
	{
		QUARTZ_FreeObj(This);
		return hr;
	}

	This->unk.pEntries = OutPinIFEntries;
	This->unk.dwEntries = sizeof(OutPinIFEntries)/sizeof(OutPinIFEntries[0]);
	This->unk.pOnFinalRelease = QUARTZ_DestroyParserOutPin;

	*ppPin = This;

	TRACE("returned successfully.\n");

	return S_OK;
}


/***************************************************************************
 *
 *	IMediaSeeking for CParserOutPinImpl
 *
 */

static HRESULT WINAPI
IMediaSeeking_fnQueryInterface(IMediaSeeking* iface,REFIID riid,void** ppobj)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	TRACE("(%p)->()\n",This);

	return IUnknown_QueryInterface(This->unk.punkControl,riid,ppobj);
}

static ULONG WINAPI
IMediaSeeking_fnAddRef(IMediaSeeking* iface)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	TRACE("(%p)->()\n",This);

	return IUnknown_AddRef(This->unk.punkControl);
}

static ULONG WINAPI
IMediaSeeking_fnRelease(IMediaSeeking* iface)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	TRACE("(%p)->()\n",This);

	return IUnknown_Release(This->unk.punkControl);
}


static HRESULT WINAPI
IMediaSeeking_fnGetCapabilities(IMediaSeeking* iface,DWORD* pdwCaps)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnCheckCapabilities(IMediaSeeking* iface,DWORD* pdwCaps)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnIsFormatSupported(IMediaSeeking* iface,const GUID* pidFormat)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnQueryPreferredFormat(IMediaSeeking* iface,GUID* pidFormat)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetTimeFormat(IMediaSeeking* iface,GUID* pidFormat)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnIsUsingTimeFormat(IMediaSeeking* iface,const GUID* pidFormat)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnSetTimeFormat(IMediaSeeking* iface,const GUID* pidFormat)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetDuration(IMediaSeeking* iface,LONGLONG* pllDuration)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	/* the following line may produce too many FIXMEs... */
	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetStopPosition(IMediaSeeking* iface,LONGLONG* pllPos)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetCurrentPosition(IMediaSeeking* iface,LONGLONG* pllPos)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnConvertTimeFormat(IMediaSeeking* iface,LONGLONG* pllOut,const GUID* pidFmtOut,LONGLONG llIn,const GUID* pidFmtIn)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnSetPositions(IMediaSeeking* iface,LONGLONG* pllCur,DWORD dwCurFlags,LONGLONG* pllStop,DWORD dwStopFlags)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetPositions(IMediaSeeking* iface,LONGLONG* pllCur,LONGLONG* pllStop)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetAvailable(IMediaSeeking* iface,LONGLONG* pllFirst,LONGLONG* pllLast)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnSetRate(IMediaSeeking* iface,double dblRate)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetRate(IMediaSeeking* iface,double* pdblRate)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaSeeking_fnGetPreroll(IMediaSeeking* iface,LONGLONG* pllPreroll)
{
	CParserOutPinImpl_THIS(iface,mediaseeking);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}




static ICOM_VTABLE(IMediaSeeking) imediaseeking =
{
	ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
	/* IUnknown fields */
	IMediaSeeking_fnQueryInterface,
	IMediaSeeking_fnAddRef,
	IMediaSeeking_fnRelease,
	/* IMediaSeeking fields */
	IMediaSeeking_fnGetCapabilities,
	IMediaSeeking_fnCheckCapabilities,
	IMediaSeeking_fnIsFormatSupported,
	IMediaSeeking_fnQueryPreferredFormat,
	IMediaSeeking_fnGetTimeFormat,
	IMediaSeeking_fnIsUsingTimeFormat,
	IMediaSeeking_fnSetTimeFormat,
	IMediaSeeking_fnGetDuration,
	IMediaSeeking_fnGetStopPosition,
	IMediaSeeking_fnGetCurrentPosition,
	IMediaSeeking_fnConvertTimeFormat,
	IMediaSeeking_fnSetPositions,
	IMediaSeeking_fnGetPositions,
	IMediaSeeking_fnGetAvailable,
	IMediaSeeking_fnSetRate,
	IMediaSeeking_fnGetRate,
	IMediaSeeking_fnGetPreroll,
};

HRESULT CParserOutPinImpl_InitIMediaSeeking( CParserOutPinImpl* This )
{
	TRACE("(%p)\n",This);
	ICOM_VTBL(&This->mediaseeking) = &imediaseeking;

	return NOERROR;
}

void CParserOutPinImpl_UninitIMediaSeeking( CParserOutPinImpl* This )
{
	TRACE("(%p)\n",This);
}

/***************************************************************************
 *
 *	IMediaPosition for CParserOutPinImpl
 *
 */

static HRESULT WINAPI
IMediaPosition_fnQueryInterface(IMediaPosition* iface,REFIID riid,void** ppobj)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	TRACE("(%p)->()\n",This);

	return IUnknown_QueryInterface(This->unk.punkControl,riid,ppobj);
}

static ULONG WINAPI
IMediaPosition_fnAddRef(IMediaPosition* iface)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	TRACE("(%p)->()\n",This);

	return IUnknown_AddRef(This->unk.punkControl);
}

static ULONG WINAPI
IMediaPosition_fnRelease(IMediaPosition* iface)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	TRACE("(%p)->()\n",This);

	return IUnknown_Release(This->unk.punkControl);
}

static HRESULT WINAPI
IMediaPosition_fnGetTypeInfoCount(IMediaPosition* iface,UINT* pcTypeInfo)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnGetTypeInfo(IMediaPosition* iface,UINT iTypeInfo, LCID lcid, ITypeInfo** ppobj)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnGetIDsOfNames(IMediaPosition* iface,REFIID riid, LPOLESTR* ppwszName, UINT cNames, LCID lcid, DISPID* pDispId)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnInvoke(IMediaPosition* iface,DISPID DispId, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarRes, EXCEPINFO* pExcepInfo, UINT* puArgErr)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}


static HRESULT WINAPI
IMediaPosition_fnget_Duration(IMediaPosition* iface,REFTIME* prefTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnput_CurrentPosition(IMediaPosition* iface,REFTIME refTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnget_CurrentPosition(IMediaPosition* iface,REFTIME* prefTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnget_StopTime(IMediaPosition* iface,REFTIME* prefTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnput_StopTime(IMediaPosition* iface,REFTIME refTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnget_PrerollTime(IMediaPosition* iface,REFTIME* prefTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnput_PrerollTime(IMediaPosition* iface,REFTIME refTime)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnput_Rate(IMediaPosition* iface,double dblRate)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	return IMediaSeeking_SetRate(CParserOutPinImpl_IMediaSeeking(This),dblRate);
}

static HRESULT WINAPI
IMediaPosition_fnget_Rate(IMediaPosition* iface,double* pdblRate)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	return IMediaSeeking_GetRate(CParserOutPinImpl_IMediaSeeking(This),pdblRate);
}

static HRESULT WINAPI
IMediaPosition_fnCanSeekForward(IMediaPosition* iface,LONG* pCanSeek)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}

static HRESULT WINAPI
IMediaPosition_fnCanSeekBackward(IMediaPosition* iface,LONG* pCanSeek)
{
	CParserOutPinImpl_THIS(iface,mediaposition);

	FIXME("(%p)->() stub!\n",This);

	return E_NOTIMPL;
}


static ICOM_VTABLE(IMediaPosition) imediaposition =
{
	ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
	/* IUnknown fields */
	IMediaPosition_fnQueryInterface,
	IMediaPosition_fnAddRef,
	IMediaPosition_fnRelease,
	/* IDispatch fields */
	IMediaPosition_fnGetTypeInfoCount,
	IMediaPosition_fnGetTypeInfo,
	IMediaPosition_fnGetIDsOfNames,
	IMediaPosition_fnInvoke,
	/* IMediaPosition fields */
	IMediaPosition_fnget_Duration,
	IMediaPosition_fnput_CurrentPosition,
	IMediaPosition_fnget_CurrentPosition,
	IMediaPosition_fnget_StopTime,
	IMediaPosition_fnput_StopTime,
	IMediaPosition_fnget_PrerollTime,
	IMediaPosition_fnput_PrerollTime,
	IMediaPosition_fnput_Rate,
	IMediaPosition_fnget_Rate,
	IMediaPosition_fnCanSeekForward,
	IMediaPosition_fnCanSeekBackward,
};


HRESULT CParserOutPinImpl_InitIMediaPosition( CParserOutPinImpl* This )
{
	TRACE("(%p)\n",This);
	ICOM_VTBL(&This->mediaposition) = &imediaposition;

	return NOERROR;
}

void CParserOutPinImpl_UninitIMediaPosition( CParserOutPinImpl* This )
{
	TRACE("(%p)\n",This);
}

