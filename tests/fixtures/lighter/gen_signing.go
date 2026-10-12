package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"github.com/elliottech/lighter-go/signer"
	"github.com/elliottech/lighter-go/types/txtypes"
	curve "github.com/elliottech/poseidon_crypto/curve/ecgfp5"
	g "github.com/elliottech/poseidon_crypto/field/goldilocks"
	gFp5 "github.com/elliottech/poseidon_crypto/field/goldilocks_quintic_extension"
	p2g "github.com/elliottech/poseidon_crypto/hash/poseidon2_goldilocks"
	p2 "github.com/elliottech/poseidon_crypto/hash/poseidon2_goldilocks_plonky2"
	schnorr "github.com/elliottech/poseidon_crypto/signature/schnorr"
)

type vec map[string]any

type txi interface {
	Hash(uint32) ([]byte, error)
	GetTxInfo() (string, error)
	GetTxType() uint8
}

func signK(hash []byte, sk, k curve.ECgFp5Scalar) string {
	m, _ := gFp5.FromCanonicalLittleEndianBytes(hash)
	return hex.EncodeToString(schnorr.SchnorrSignHashedMessage2(m, sk, k).ToBytes())
}

func main() {
	out := []vec{}
	skBytes, _ := hex.DecodeString("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728")
	km, err := signer.NewKeyManager(skBytes)
	if err != nil {
		panic(err)
	}
	sk := curve.ScalarElementFromLittleEndianBytes(skBytes)
	pk := km.PubKeyBytes()
	kBytes, _ := hex.DecodeString("a1a2a3a4a5a6a7a8b1b2b3b4b5b6b7b8c1c2c3c4c5c6c7c8d1d2d3d4d5d6d7d8e1e2e3e4e5e6e7e8")
	k := curve.ScalarElementFromLittleEndianBytes(kBytes)
	out = append(out, vec{"kind": "key", "sk": hex.EncodeToString(sk.ToLittleEndianBytes()), "pk": hex.EncodeToString(pk[:]),
		"k": hex.EncodeToString(k.ToLittleEndianBytes()),
		"kG": hex.EncodeToString(curve.GENERATOR_ECgFp5Point.Mul(k).Encode().ToLittleEndianBytes())})
	seed := "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
	seedKm, _ := signer.NewSeedKeyManager(seed)
	spk := seedKm.PubKeyBytes()
	out = append(out, vec{"kind": "seed", "seed": seed, "sk": hex.EncodeToString(seedKm.PrvKeyBytes()), "pk": hex.EncodeToString(spk[:])})
	for _, n := range []int{1, 5, 8, 9, 16, 17} {
		in := make([]g.GoldilocksField, n)
		u := []uint64{}
		for i := range in {
			in[i] = g.GoldilocksField(uint64(i)*0x9E3779B97F4A7C15 + 7)
			u = append(u, uint64(in[i]))
		}
		h := p2.HashToQuinticExtension(in)
		out = append(out, vec{"kind": "hash", "in": u, "out": hex.EncodeToString(h.ToLittleEndianBytes())})
	}
	chain := uint32(304)
	add := func(name string, tx txi, setSig func([]byte)) {
		h, err := tx.Hash(chain)
		if err != nil {
			panic(err)
		}
		sigHex := signK(h, sk, k)
		sig, _ := hex.DecodeString(sigHex)
		setSig(sig)
		info, err := tx.GetTxInfo()
		if err != nil {
			panic(err)
		}
		out = append(out, vec{"kind": name, "type": tx.GetTxType(), "hash": hex.EncodeToString(h), "sig": sigHex, "info": info})
	}
	co := &txtypes.L2CreateOrderTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3,
		OrderInfo: &txtypes.OrderInfo{MarketIndex: 110, ClientOrderIndex: 123456789012, BaseAmount: 87, Price: 2312345, IsAsk: 1,
			Type: txtypes.LimitOrder, TimeInForce: txtypes.PostOnly, ReduceOnly: 0, TriggerPrice: 0, OrderExpiry: 1793000000000},
		ExpiredAt: 1791800000000, Nonce: 77}
	add("create_order", co, func(s []byte) { co.Sig = s })
	co2 := &txtypes.L2CreateOrderTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3,
		OrderInfo: &txtypes.OrderInfo{MarketIndex: 1, ClientOrderIndex: 5, BaseAmount: 1000, Price: 900000, IsAsk: 0,
			Type: txtypes.LimitOrder, TimeInForce: txtypes.ImmediateOrCancel, ReduceOnly: 1, TriggerPrice: 0, OrderExpiry: 0},
		ExpiredAt: 1791800000001, Nonce: 78, L2TxAttributes: txtypes.L2TxAttributes{txtypes.AttributeTypeSkipTxNonce: 1}}
	add("create_order_ioc_skipnonce", co2, func(s []byte) { co2.Sig = s })
	cx := &txtypes.L2CancelOrderTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3, MarketIndex: 110, Index: 123456789012,
		ExpiredAt: 1791800000002, Nonce: 79}
	add("cancel_order", cx, func(s []byte) { cx.Sig = s })
	md := &txtypes.L2ModifyOrderTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3, MarketIndex: 110, Index: 123456789012,
		BaseAmount: 90, Price: 2312000, TriggerPrice: 0, ExpiredAt: 1791800000003, Nonce: 80}
	add("modify_order", md, func(s []byte) { md.Sig = s })
	ca := &txtypes.L2CancelAllOrdersTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3, TimeInForce: txtypes.ScheduledCancelAll,
		Time: 1791800600000, ExpiredAt: 1791800000004, Nonce: 81}
	add("cancel_all_scheduled", ca, func(s []byte) { ca.Sig = s })
	ca2 := &txtypes.L2CancelAllOrdersTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3, TimeInForce: txtypes.ImmediateCancelAll,
		Time: 0, ExpiredAt: 1791800000005, Nonce: 82}
	add("cancel_all_now", ca2, func(s []byte) { ca2.Sig = s })
	cp := &txtypes.L2ChangePubKeyTxInfo{AccountIndex: 281474976499586, ApiKeyIndex: 3, PubKey: pk[:], ExpiredAt: 1791800000006, Nonce: 0}
	add("change_pub_key", cp, func(s []byte) { cp.Sig = s })
	out = append(out, vec{"kind": "change_pub_key_l1_body", "body": cp.GetL1SignatureBody()})
	msg := fmt.Sprintf("%v:%v:%v", int64(1791800600), int64(281474976499586), 3)
	arr, _ := g.ArrayFromCanonicalLittleEndianBytes([]byte(msg))
	ah := p2g.HashToQuinticExtension(arr).ToLittleEndianBytes()
	out = append(out, vec{"kind": "auth", "msg": msg, "hash": hex.EncodeToString(ah), "sig": signK(ah, sk, k)})
	b, _ := json.MarshalIndent(out, "", " ")
	os.Stdout.Write(b)
}
