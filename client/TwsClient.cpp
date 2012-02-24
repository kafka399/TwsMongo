#include "TwsClient.h"

#include "EPosixClientSocket.h"
#include "EPosixClientSocketPlatform.h"

#include "Contract.h"
#include "Order.h"

const int PING_DEADLINE = 2; // seconds
const int SLEEP_BETWEEN_PINGS = 30; // seconds

///////////////////////////////////////////////////////////
// member funcs
TwsClient::TwsClient()
	: m_pClient(new EPosixClientSocket(this))
	, m_state(ST_CONNECT)
	, m_sleepDeadline(0)
	, m_orderId(0)
{
}

TwsClient::~TwsClient()
{
}

bool TwsClient::connect(const char *host, unsigned int port, int clientId)
{
	// trying to connect
	printf( "Connecting to %s:%d clientId:%d\n", !( host && *host) ? "127.0.0.1" : host, port, clientId);

	bool bRes = m_pClient->eConnect( host, port, clientId);

	if (bRes) {
		printf( "Connected to %s:%d clientId:%d\n", !( host && *host) ? "127.0.0.1" : host, port, clientId);
	}
	else
		printf( "Cannot connect to %s:%d clientId:%d\n", !( host && *host) ? "127.0.0.1" : host, port, clientId);

	return bRes;
}

void TwsClient::disconnect() const
{
	m_pClient->eDisconnect();

	printf ( "Disconnected\n");
}

bool TwsClient::isConnected() const
{
	return m_pClient->isConnected();
}

void TwsClient::processMessages()
{
	fd_set readSet, writeSet, errorSet;

	struct timeval tval;
	tval.tv_usec = 0;
	tval.tv_sec = 0;

	time_t now = time(NULL);
	//std::cout<<"m_state "<<m_state<<std::endl;

	switch (m_state) {
		case ST_PLACEORDER:
			placeOrder();
			break;
		case ST_PLACEORDER_ACK:
			break;
		case ST_CANCELORDER:
			cancelOrder();
			break;
		case ST_CANCELORDER_ACK:
			break;
		case ST_PING:
			reqCurrentTime();
			break;
		case ST_PING_ACK:
			if( m_sleepDeadline < now) {
				disconnect();
				return;
			}
			break;
		case ST_IDLE:
			if( m_sleepDeadline < now) {
				m_state = ST_PING;
				return;
			}
			break;
		case ST_MKT_DATA:
			reqMktData();
	}

	if( m_sleepDeadline > 0) {
		// initialize timeout with m_sleepDeadline - now
		tval.tv_sec = m_sleepDeadline - now;
	}

	if( m_pClient->fd() >= 0 ) {

		FD_ZERO( &readSet);
		errorSet = writeSet = readSet;

		FD_SET( m_pClient->fd(), &readSet);

		if( !m_pClient->isOutBufferEmpty())
			FD_SET( m_pClient->fd(), &writeSet);

		FD_CLR( m_pClient->fd(), &errorSet);

		int ret = select( m_pClient->fd() + 1, &readSet, &writeSet, &errorSet, &tval);

		if( ret == 0) { // timeout
			return;
		}

		if( ret < 0) {	// error
			disconnect();
			return;
		}

		if( m_pClient->fd() < 0)
			return;

		if( FD_ISSET( m_pClient->fd(), &errorSet)) {
			// error on socket
			m_pClient->onError();
		}

		if( m_pClient->fd() < 0)
			return;

		if( FD_ISSET( m_pClient->fd(), &writeSet)) {
			// socket is ready for writing
			m_pClient->onSend();
		}

		if( m_pClient->fd() < 0)
			return;

		if( FD_ISSET( m_pClient->fd(), &readSet)) {
			// socket is ready for reading
			m_pClient->onReceive();
		}
	}
}

//////////////////////////////////////////////////////////////////
// methods
void TwsClient::reqMktData()
{
	Contract contract;
	contract.symbol = "MSFT";
	contract.secType = "STK";
	contract.exchange = "SMART";
	contract.currency = "USD";
	m_pClient->reqMktData(1, contract, "", false);
	m_state = ST_PING;
}

void TwsClient::reqCurrentTime()
{
	printf( "Requesting Current Time\n");

	// set ping deadline to "now + n seconds"
	m_sleepDeadline = time( NULL) + PING_DEADLINE;

	m_state = ST_PING_ACK;

	m_pClient->reqCurrentTime();

}

void TwsClient::placeOrder()
{
	Contract contract;
	Order order;

	contract.symbol = "MSFT";
	contract.secType = "STK";
	contract.exchange = "SMART";
	contract.currency = "USD";

	order.action = "BUY";
	order.totalQuantity = 1000;
	order.orderType = "LMT";
	order.lmtPrice = 0.01;

	printf( "Placing Order %ld: %s %ld %s at %f\n", m_orderId, order.action.c_str(), order.totalQuantity, contract.symbol.c_str(), order.lmtPrice);

	m_state = ST_PLACEORDER_ACK;

	m_pClient->placeOrder( m_orderId, contract, order);
}

void TwsClient::cancelOrder()
{
	printf( "Cancelling Order %ld\n", m_orderId);

	m_state = ST_CANCELORDER_ACK;

	m_pClient->cancelOrder( m_orderId);
}

///////////////////////////////////////////////////////////////////
// events
void TwsClient::orderStatus( OrderId orderId, const IBString &status, int filled,
	   int remaining, double avgFillPrice, int permId, int parentId,
	   double lastFillPrice, int clientId, const IBString& whyHeld)

{
	if( orderId == m_orderId) {
		if( m_state == ST_PLACEORDER_ACK && (status == "PreSubmitted" || status == "Submitted"))
			m_state = ST_CANCELORDER;

		if( m_state == ST_CANCELORDER_ACK && status == "Cancelled")
			m_state = ST_PING;

		printf( "Order: id=%ld, status=%s\n", orderId, status.c_str());
	}
}

void TwsClient::nextValidId( OrderId orderId)
{
	m_orderId = orderId;

	m_state = ST_MKT_DATA;
/*
	Contract contract;
	contract.symbol = "MSFT";
	contract.secType = "STK";
	contract.exchange = "SMART";
	contract.currency = "USD";

	m_pClient->reqMktData(1, contract, "", false);
	*/
}

void TwsClient::currentTime( long time)
{
	if ( m_state == ST_PING_ACK) {
		time_t t = ( time_t)time;
		struct tm * timeinfo = localtime ( &t);
		printf( "The current date/time is: %s", asctime( timeinfo));

		time_t now = ::time(NULL);
		m_sleepDeadline = now + SLEEP_BETWEEN_PINGS;

		m_state = ST_IDLE;
	}
}

void TwsClient::error(const int id, const int errorCode, const IBString errorString)
{
//	printf( "Error id=%d, errorCode=%d, msg=%s\n", id, errorCode, errorString.c_str());

	if( id == -1 && errorCode == 1100) // if "Connectivity between IB and TWS has been lost"
		disconnect();
}

void TwsClient::tickPrice( TickerId tickerId, TickType field, double price, int canAutoExecute) {
	gettimeofday(&start, NULL);
	mongo::ScopedDbConnection conn("localhost");

	mongo::BSONObj p =
			mongo::BSONObjBuilder().genOID().append("tickerId",(long long)tickerId).append("field",field).append("price",price).appendNumber("tstamp",(long long)(start.tv_sec*1000+(int)(start.tv_usec/1000))).obj();
	conn.get()->insert("quotes.test", p);
	conn.done();
	//std::cout<<"id "<<tickerId<<" field "<<field<<" price "<<price<<std::endl;
}

void TwsClient::tickSize( TickerId tickerId, TickType field, int size) {
	gettimeofday(&start, NULL);
	mongo::ScopedDbConnection conn("localhost");
	mongo::BSONObj p =
			mongo::BSONObjBuilder().genOID().append("tickerId",(long long)tickerId).append("field",field).append("size",size).appendNumber("tstamp",(long long)(start.tv_sec*1000+(int)(start.tv_usec/1000))).obj();
	conn.get()->insert("quotes.test", p);
	conn.done();

	std::cout<<"id "<<tickerId<<" field "<<field<<" size "<<size<<std::endl;
}

void TwsClient::tickOptionComputation( TickerId tickerId, TickType tickType, double impliedVol, double delta,
											 double optPrice, double pvDividend,
											 double gamma, double vega, double theta, double undPrice) {}
void TwsClient::tickGeneric(TickerId tickerId, TickType tickType, double value) {}
void TwsClient::tickString(TickerId tickerId, TickType tickType, const IBString& value) {}
void TwsClient::tickEFP(TickerId tickerId, TickType tickType, double basisPoints, const IBString& formattedBasisPoints,
							   double totalDividends, int holdDays, const IBString& futureExpiry, double dividendImpact, double dividendsToExpiry) {}
void TwsClient::openOrder( OrderId orderId, const Contract&, const Order&, const OrderState& ostate) {}
void TwsClient::openOrderEnd() {}
void TwsClient::winError( const IBString &str, int lastError) {}
void TwsClient::connectionClosed() {}
void TwsClient::updateAccountValue(const IBString& key, const IBString& val,
										  const IBString& currency, const IBString& accountName) {}
void TwsClient::updatePortfolio(const Contract& contract, int position,
		double marketPrice, double marketValue, double averageCost,
		double unrealizedPNL, double realizedPNL, const IBString& accountName){}
void TwsClient::updateAccountTime(const IBString& timeStamp) {}
void TwsClient::accountDownloadEnd(const IBString& accountName) {}
void TwsClient::contractDetails( int reqId, const ContractDetails& contractDetails) {}
void TwsClient::bondContractDetails( int reqId, const ContractDetails& contractDetails) {}
void TwsClient::contractDetailsEnd( int reqId) {}
void TwsClient::execDetails( int reqId, const Contract& contract, const Execution& execution) {}
void TwsClient::execDetailsEnd( int reqId) {}

void TwsClient::updateMktDepth(TickerId id, int position, int operation, int side,
									  double price, int size) {}
void TwsClient::updateMktDepthL2(TickerId id, int position, IBString marketMaker, int operation,
										int side, double price, int size) {}
void TwsClient::updateNewsBulletin(int msgId, int msgType, const IBString& newsMessage, const IBString& originExch) {}
void TwsClient::managedAccounts( const IBString& accountsList) {}
void TwsClient::receiveFA(faDataType pFaDataType, const IBString& cxml) {}
void TwsClient::historicalData(TickerId reqId, const IBString& date, double open, double high,
									  double low, double close, int volume, int barCount, double WAP, int hasGaps) {}
void TwsClient::scannerParameters(const IBString &xml) {}
void TwsClient::scannerData(int reqId, int rank, const ContractDetails &contractDetails,
	   const IBString &distance, const IBString &benchmark, const IBString &projection,
	   const IBString &legsStr) {}
void TwsClient::scannerDataEnd(int reqId) {}
void TwsClient::realtimeBar(TickerId reqId, long time, double open, double high, double low, double close,
								   long volume, double wap, int count) {}
void TwsClient::fundamentalData(TickerId reqId, const IBString& data) {}
void TwsClient::deltaNeutralValidation(int reqId, const UnderComp& underComp) {}
void TwsClient::tickSnapshotEnd(int reqId) {}
void TwsClient::marketDataType(TickerId reqId, int marketDataType) {}

