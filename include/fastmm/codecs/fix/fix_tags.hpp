#pragma once
// FIX 4.4 tag numbers, message types and enumeration values used by fastmm::codecs::fix.
// There is no XML dictionary: every constant the codec reads or writes is listed here, and
// each was checked against the FIX 4.4 dictionary (docs/reference/codecs/fix.md, "Sources"):
//   * FIX Trading Community, FIX 4.4 specification with Errata 20030618
//     https://www.fixtrading.org/standards/fix-4-4/
//   * OnixS FIX 4.4 dictionary, https://www.onixs.biz/fix-dictionary/4.4/ (tagNum_<n>.html and
//     msgType_<t>_<n>.html pages for every tag and message below)
//   * B2BITS FIXopaedia FIX 4.4, https://www.b2bits.com/fixopaedia/fixdic44/data_types.html
#include <cstdint>
#include <string_view>

namespace fastmm::codecs::fix {

inline constexpr char kSoh = '\x01';
inline constexpr std::string_view kBeginString44 = "FIX.4.4";

namespace tag {
inline constexpr std::uint32_t kAvgPx = 6;
inline constexpr std::uint32_t kBeginSeqNo = 7;
inline constexpr std::uint32_t kBeginString = 8;
inline constexpr std::uint32_t kBodyLength = 9;
inline constexpr std::uint32_t kCheckSum = 10;
inline constexpr std::uint32_t kClOrdID = 11;
inline constexpr std::uint32_t kCumQty = 14;
inline constexpr std::uint32_t kEndSeqNo = 16;
inline constexpr std::uint32_t kExecID = 17;
inline constexpr std::uint32_t kExecInst = 18;
inline constexpr std::uint32_t kLastPx = 31;
inline constexpr std::uint32_t kLastQty = 32;
inline constexpr std::uint32_t kMsgSeqNum = 34;
inline constexpr std::uint32_t kMsgType = 35;
inline constexpr std::uint32_t kNewSeqNo = 36;
inline constexpr std::uint32_t kOrderID = 37;
inline constexpr std::uint32_t kOrderQty = 38;
inline constexpr std::uint32_t kOrdStatus = 39;
inline constexpr std::uint32_t kOrdType = 40;
inline constexpr std::uint32_t kOrigClOrdID = 41;
inline constexpr std::uint32_t kPossDupFlag = 43;
inline constexpr std::uint32_t kPrice = 44;
inline constexpr std::uint32_t kRefSeqNum = 45;
inline constexpr std::uint32_t kSenderCompID = 49;
inline constexpr std::uint32_t kSendingTime = 52;
inline constexpr std::uint32_t kSide = 54;
inline constexpr std::uint32_t kSymbol = 55;
inline constexpr std::uint32_t kTargetCompID = 56;
inline constexpr std::uint32_t kText = 58;
inline constexpr std::uint32_t kTimeInForce = 59;
inline constexpr std::uint32_t kTransactTime = 60;
inline constexpr std::uint32_t kPossResend = 97;
inline constexpr std::uint32_t kEncryptMethod = 98;
inline constexpr std::uint32_t kCxlRejReason = 102;
inline constexpr std::uint32_t kOrdRejReason = 103;
inline constexpr std::uint32_t kHeartBtInt = 108;
inline constexpr std::uint32_t kTestReqID = 112;
inline constexpr std::uint32_t kOrigSendingTime = 122;
inline constexpr std::uint32_t kGapFillFlag = 123;
inline constexpr std::uint32_t kResetSeqNumFlag = 141;
inline constexpr std::uint32_t kExecType = 150;
inline constexpr std::uint32_t kLeavesQty = 151;
inline constexpr std::uint32_t kMDReqID = 262;
inline constexpr std::uint32_t kNoMDEntries = 268;
inline constexpr std::uint32_t kMDEntryType = 269;
inline constexpr std::uint32_t kMDEntryPx = 270;
inline constexpr std::uint32_t kMDEntrySize = 271;
inline constexpr std::uint32_t kMDEntryID = 278;
inline constexpr std::uint32_t kMDUpdateAction = 279;
inline constexpr std::uint32_t kRefTagID = 371;
inline constexpr std::uint32_t kRefMsgType = 372;
inline constexpr std::uint32_t kSessionRejectReason = 373;
inline constexpr std::uint32_t kCxlRejResponseTo = 434;
inline constexpr std::uint32_t kLastLiquidityInd = 851;
}  // namespace tag

// MsgType(35). Session (administrative) messages first.
namespace msg {
inline constexpr std::string_view kHeartbeat = "0";
inline constexpr std::string_view kTestRequest = "1";
inline constexpr std::string_view kResendRequest = "2";
inline constexpr std::string_view kReject = "3";
inline constexpr std::string_view kSequenceReset = "4";
inline constexpr std::string_view kLogout = "5";
inline constexpr std::string_view kLogon = "A";
inline constexpr std::string_view kExecutionReport = "8";
inline constexpr std::string_view kOrderCancelReject = "9";
inline constexpr std::string_view kNewOrderSingle = "D";
inline constexpr std::string_view kOrderCancelRequest = "F";
inline constexpr std::string_view kOrderCancelReplaceRequest = "G";
inline constexpr std::string_view kMarketDataSnapshotFullRefresh = "W";
inline constexpr std::string_view kMarketDataIncrementalRefresh = "X";
}  // namespace msg

// Session-level message types (FIX 4.4 session protocol): everything else is application.
[[nodiscard]] constexpr bool is_admin_msg_type(std::string_view t) noexcept {
  return t.size() == 1 && (t[0] == '0' || t[0] == '1' || t[0] == '2' || t[0] == '3' ||
                           t[0] == '4' || t[0] == '5' || t[0] == 'A');
}

// ExecType(150), FIX 4.4. '1'/'2' (Partial fill / Fill) were replaced by 'F' (Trade) in FIX 4.3;
// the decoder still accepts them from older counterparties.
namespace exec_type {
inline constexpr char kNew = '0';
inline constexpr char kPartialFillLegacy = '1';
inline constexpr char kFillLegacy = '2';
inline constexpr char kDoneForDay = '3';
inline constexpr char kCanceled = '4';
inline constexpr char kReplaced = '5';
inline constexpr char kPendingCancel = '6';
inline constexpr char kRejected = '8';
inline constexpr char kPendingNew = 'A';
inline constexpr char kExpired = 'C';
inline constexpr char kRestated = 'D';
inline constexpr char kPendingReplace = 'E';
inline constexpr char kTrade = 'F';
inline constexpr char kTradeCorrect = 'G';
inline constexpr char kTradeCancel = 'H';
inline constexpr char kOrderStatus = 'I';
}  // namespace exec_type

// OrdStatus(39), FIX 4.4.
namespace ord_status {
inline constexpr char kNew = '0';
inline constexpr char kPartiallyFilled = '1';
inline constexpr char kFilled = '2';
inline constexpr char kDoneForDay = '3';
inline constexpr char kCanceled = '4';
inline constexpr char kPendingCancel = '6';
inline constexpr char kRejected = '8';
inline constexpr char kPendingNew = 'A';
inline constexpr char kExpired = 'C';
inline constexpr char kPendingReplace = 'E';
}  // namespace ord_status

// Side(54): 1 = Buy, 2 = Sell.
inline constexpr char kSideBuy = '1';
inline constexpr char kSideSell = '2';
// OrdType(40): 1 = Market, 2 = Limit.
inline constexpr char kOrdTypeMarket = '1';
inline constexpr char kOrdTypeLimit = '2';
// TimeInForce(59): 0 = Day, 1 = GTC, 3 = IOC, 4 = FOK (absent means Day).
inline constexpr char kTifDay = '0';
inline constexpr char kTifGtc = '1';
inline constexpr char kTifIoc = '3';
inline constexpr char kTifFok = '4';
// ExecInst(18) is a MultipleValueString (space separated); 6 = Participate don't initiate.
inline constexpr char kExecInstParticipateDontInitiate = '6';
// MDEntryType(269): 0 = Bid, 1 = Offer, 2 = Trade.
inline constexpr char kMdBid = '0';
inline constexpr char kMdOffer = '1';
inline constexpr char kMdTrade = '2';
// MDUpdateAction(279): 0 = New, 1 = Change, 2 = Delete.
inline constexpr char kMdNew = '0';
inline constexpr char kMdChange = '1';
inline constexpr char kMdDelete = '2';
// LastLiquidityInd(851): 1 = Added Liquidity, 2 = Removed Liquidity, 3 = Liquidity Routed Out.
inline constexpr std::int64_t kLiquidityAdded = 1;
inline constexpr std::int64_t kLiquidityRemoved = 2;
// CxlRejResponseTo(434): 1 = Order Cancel Request, 2 = Order Cancel/Replace Request.
inline constexpr char kCxlRejToCancel = '1';
inline constexpr char kCxlRejToReplace = '2';
// CxlRejReason(102): 0 = Too late to cancel, 1 = Unknown order, 99 = Other.
inline constexpr std::int64_t kCxlRejTooLate = 0;
inline constexpr std::int64_t kCxlRejUnknownOrder = 1;
inline constexpr std::int64_t kCxlRejOther = 99;
// OrdRejReason(103), FIX 4.4.
namespace ord_rej {
inline constexpr std::int64_t kBrokerOption = 0;
inline constexpr std::int64_t kUnknownSymbol = 1;
inline constexpr std::int64_t kExchangeClosed = 2;
inline constexpr std::int64_t kOrderExceedsLimit = 3;
inline constexpr std::int64_t kUnknownOrder = 5;
inline constexpr std::int64_t kDuplicateOrder = 6;
inline constexpr std::int64_t kUnsupportedOrderCharacteristic = 11;
inline constexpr std::int64_t kIncorrectQuantity = 13;
inline constexpr std::int64_t kOther = 99;
}  // namespace ord_rej

// SessionRejectReason(373), FIX 4.4.
namespace session_reject {
inline constexpr std::int64_t kInvalidTagNumber = 0;
inline constexpr std::int64_t kRequiredTagMissing = 1;
inline constexpr std::int64_t kTagSpecifiedWithoutValue = 4;
inline constexpr std::int64_t kValueIncorrect = 5;
inline constexpr std::int64_t kIncorrectDataFormat = 6;
inline constexpr std::int64_t kCompIdProblem = 9;
inline constexpr std::int64_t kSendingTimeAccuracyProblem = 10;
inline constexpr std::int64_t kInvalidMsgType = 11;
inline constexpr std::int64_t kIncorrectNumInGroupCount = 16;
inline constexpr std::int64_t kOther = 99;
}  // namespace session_reject

}  // namespace fastmm::codecs::fix
