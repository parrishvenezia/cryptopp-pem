// pem-wr.cpp - PEM write routines.
//              Written and placed in the public domain by Jeffrey Walton

///////////////////////////////////////////////////////////////////////////
// For documentation on the PEM read and write routines, see
//   http://www.cryptopp.com/wiki/PEM_Pack
///////////////////////////////////////////////////////////////////////////

#include <string>
using std::string;

#include <algorithm>
using std::transform;

#include <cctype>

#include "cryptlib.h"
#include "secblock.h"
#include "camellia.h"
#include "smartptr.h"
#include "filters.h"
#include "base64.h"
#include "files.h"
#include "queue.h"
#include "modes.h"
#include "osrng.h"
#include "asn.h"
#include "aes.h"
#include "idea.h"
#include "des.h"
#include "hex.h"

#include "pem.h"
#include "pem_common.h"

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include "md5.h"

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

ANONYMOUS_NAMESPACE_BEGIN

using namespace CryptoPP;

// Returns a keyed StreamTransformation ready to use to encrypt a DER encoded key
void PEM_CipherForAlgorithm(RandomNumberGenerator& rng, string algorithm, member_ptr<StreamTransformation>& stream,
                            SecByteBlock& key, SecByteBlock& iv, const char* password, size_t length);

void PEM_DEREncode(BufferedTransformation& bt, const PKCS8PrivateKey& key);
void PEM_DEREncode(BufferedTransformation& bt, const X509PublicKey& key);

// Ambiguous call; needs a best match. Provide an overload.
void PEM_DEREncode(BufferedTransformation& bt, const RSA::PrivateKey& key);

// Special handling for DSA private keys. Crypto++ provides {version,x},
//   while OpenSSL expects {version,p,q,g,y,x}.
void PEM_DEREncode(BufferedTransformation& bt, const DSA::PrivateKey& key);

// Special handling for EC private keys. Crypto++ provides {version,x},
//   while OpenSSL expects {version,x,curve oid,y}.
template <class EC>
void PEM_DEREncode(BufferedTransformation& bt, const DL_PrivateKey_EC<EC>& key);

void PEM_Encrypt(BufferedTransformation& src, BufferedTransformation& dest, member_ptr<StreamTransformation>& stream);
void PEM_EncryptAndEncode(BufferedTransformation& src, BufferedTransformation& dest, member_ptr<StreamTransformation>& stream);

template <class EC>
void PEM_SaveParams(BufferedTransformation& bt, const DL_GroupParameters_EC< EC >& params, const SecByteBlock& pre, const SecByteBlock& post);

template <class KEY>
void PEM_SaveKey(BufferedTransformation& bt,
                        const KEY& key, const SecByteBlock& pre, const SecByteBlock& post);

template <class PUBLIC_KEY>
void PEM_SavePublicKey(BufferedTransformation& bt,
                       const PUBLIC_KEY& key, const SecByteBlock& pre, const SecByteBlock& post);

template <class PRIVATE_KEY>
void PEM_SavePrivateKey(BufferedTransformation& bt,
                        const PRIVATE_KEY& key, const SecByteBlock& pre, const SecByteBlock& post);

template <class PRIVATE_KEY>
void PEM_SavePrivateKey(BufferedTransformation& bt, RandomNumberGenerator& rng,
                        const PRIVATE_KEY& key, const SecByteBlock& pre, const SecByteBlock& post,
                        const std::string& algorithm, const char* password, size_t length);

// Fetches and sets encodeAsOID parameter
template <class EC>
bool PEM_GetNamedCurve(const DL_GroupParameters_EC<EC>& params);

// Fetches and sets encodeAsOID parameter
template <class EC>
void PEM_SetNamedCurve(const DL_GroupParameters_EC<EC>& params, bool flag);

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

template <class EC>
void PEM_SaveParams(BufferedTransformation& bt, const DL_GroupParameters_EC< EC >& params, const SecByteBlock& pre, const SecByteBlock& post)
{
    PEM_WriteLine(bt, pre);

    Base64Encoder encoder(new Redirector(bt), true /*lineBreak*/, RFC1421_LINE_BREAK);

    params.DEREncode(encoder);
    encoder.MessageEnd();

    PEM_WriteLine(bt, post);

    bt.MessageEnd();
}

template <class EC>
void PEM_SavePrivateKey(BufferedTransformation& bt, const DL_PrivateKey_EC<EC>& key, const SecByteBlock& pre, const SecByteBlock& post)
{
    PEM_WriteLine(bt, pre);

    ByteQueue queue;
    PEM_DEREncode(queue, key);

    PEM_Base64Encode(queue, bt);

    PEM_WriteLine(bt, post);

    bt.MessageEnd();
}

void PEM_DEREncode(BufferedTransformation& bt, const DSA::PrivateKey& key)
{
    // Crypto++ provides {version,x}, while OpenSSL expects {version,p,q,g,y,x}.

    const DL_GroupParameters_DSA& params = key.GetGroupParameters();

    DSA::PublicKey pkey;
    key.MakePublicKey(pkey);

    DERSequenceEncoder seq(bt);
        DEREncodeUnsigned<word32>(seq, 0);         // version
        params.GetModulus().DEREncode(seq);        // p
        params.GetSubgroupOrder().DEREncode(seq);  // q
        params.GetGenerator().DEREncode(seq);      // g
        pkey.GetPublicElement().DEREncode(seq);    // y
        key.GetPrivateExponent().DEREncode(seq);   // x
    seq.MessageEnd();
}

template <class EC>
void PEM_DEREncode(BufferedTransformation& bt, const DL_PrivateKey_EC<EC>& key)
{
    // Crypto++ provides {version,x}, while OpenSSL expects {version,x,curve oid,y}.

    // Need a public key to encode the public element.
    DL_PublicKey_EC<EC> pkey;
    key.MakePublicKey(pkey);

    // Prefetch the group parameters
    const DL_GroupParameters_EC<EC>& params = pkey.GetGroupParameters();
    const Integer& x = key.GetPrivateExponent();

    // Named curve
    OID oid;
    if(!key.GetVoidValue(Name::GroupOID(), typeid(oid), &oid))
        throw Exception(Exception::OTHER_ERROR, "PEM_DEREncode: failed to retrieve curve OID");

    DERSequenceEncoder seq(bt);
        DEREncodeUnsigned<word32>(seq, 1);  // version
        x.DEREncodeAsOctetString(seq, params.GetSubgroupOrder().ByteCount());

        DERGeneralEncoder cs1(seq, CONTEXT_SPECIFIC | CONSTRUCTED | 0);
            oid.DEREncode(cs1);
        cs1.MessageEnd();

        DERGeneralEncoder cs2(seq, CONTEXT_SPECIFIC | CONSTRUCTED | 1);
            DERGeneralEncoder cs3(cs2, BIT_STRING);
                cs3.Put(0x00);        // Unused bits
                params.GetCurve().EncodePoint(cs3, pkey.GetPublicElement(), false);
            cs3.MessageEnd();
        cs2.MessageEnd();
    seq.MessageEnd();
    bt.MessageEnd();
}

void PEM_DEREncode(BufferedTransformation& bt, const PKCS8PrivateKey& key)
{
    key.DEREncodePrivateKey(bt);
    bt.MessageEnd();
}

void PEM_DEREncode(BufferedTransformation& bt, const X509PublicKey& key)
{
    key.DEREncode(bt);
    bt.MessageEnd();
}

void PEM_DEREncode(BufferedTransformation& bt, const RSA::PrivateKey& key)
{
    return PEM_DEREncode(bt, dynamic_cast<const PKCS8PrivateKey&>(key));
}

template <class PUBLIC_KEY>
void PEM_SavePublicKey(BufferedTransformation& bt,
                       const PUBLIC_KEY& key, const SecByteBlock& pre, const SecByteBlock& post)
{
    PEM_SaveKey(bt, key, pre, post);
}

template <class PRIVATE_KEY>
void PEM_SavePrivateKey(BufferedTransformation& bt,
                               const PRIVATE_KEY& key, const SecByteBlock& pre, const SecByteBlock& post)
{
    PEM_SaveKey(bt, key, pre, post);
}

template <class KEY>
void PEM_SaveKey(BufferedTransformation& bt, const KEY& key, const SecByteBlock& pre, const SecByteBlock& post)
{
    PEM_WriteLine(bt, pre);

    ByteQueue queue;
    PEM_DEREncode(queue, key);

    PEM_Base64Encode(queue, bt);

    PEM_WriteLine(bt, post);

    bt.MessageEnd();
}

template<class PRIVATE_KEY>
void PEM_SavePrivateKey(BufferedTransformation& bt, RandomNumberGenerator& rng,
                        const PRIVATE_KEY& key, const SecByteBlock& pre, const SecByteBlock& post,
                        const std::string& algorithm, const char* password, size_t length)
{
    ByteQueue queue;

    PEM_WriteLine(queue, pre);

    // Proc-Type: 4,ENCRYPTED
    PEM_WriteLine(queue, PROC_TYPE_ENC);

    SecByteBlock _key, _iv;
    member_ptr<StreamTransformation> stream;

    // After this executes, we have a StreamTransformation keyed and ready to go.
    PEM_CipherForAlgorithm(rng, algorithm, stream, _key, _iv, password, length);

    // Encode the IV. It gets written to the encapsulated header.
    string encoded;
    HexEncoder hex(new StringSink(encoded));
    hex.Put(_iv.data(), _iv.size());
    hex.MessageEnd();

    // e.g., DEK-Info: AES-128-CBC,5E537774BCCD88B3E2F47FE294C93253
    string line;
    line += "DEK-Info: ";
    line += algorithm + "," + encoded;

    // The extra newline separates the control fields from the encapsulated
    //   text (i.e, header from body). Its required by RFC 1421.
    PEM_WriteLine(queue, line);
    queue.Put(reinterpret_cast<const byte*>(RFC1421_EOL.data()), RFC1421_EOL.size());

    ByteQueue temp;
    PEM_DEREncode(temp, key);

    PEM_EncryptAndEncode(temp, queue, stream);

    PEM_WriteLine(queue, post);

    queue.TransferTo(bt);
    bt.MessageEnd();
}

void PEM_CipherForAlgorithm(RandomNumberGenerator& rng, string algorithm, member_ptr<StreamTransformation>& stream,
                            SecByteBlock& key, SecByteBlock& iv, const char* password, size_t length)
{
    unsigned int ksize, vsize;
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(), (int(*)(int))std::toupper);

    if(algorithm == "AES-256-CBC")
    {
        ksize = 32;
        vsize = 16;

        stream.reset(new CBC_Mode<AES>::Encryption);
    }
    else if(algorithm == "AES-192-CBC")
    {
        ksize = 24;
        vsize = 16;

        stream.reset(new CBC_Mode<AES>::Encryption);
    }
    else if(algorithm == "AES-128-CBC")
    {
        ksize = 16;
        vsize = 16;

        stream.reset(new CBC_Mode<AES>::Encryption);
    }
    else if(algorithm == "CAMELLIA-256-CBC")
    {
        ksize = 32;
        vsize = 16;

        stream.reset(new CBC_Mode<Camellia>::Encryption);
    }
    else if(algorithm == "CAMELLIA-192-CBC")
    {
        ksize = 24;
        vsize = 16;

        stream.reset(new CBC_Mode<Camellia>::Encryption);
    }
    else if(algorithm == "CAMELLIA-128-CBC")
    {
        ksize = 16;
        vsize = 16;

        stream.reset(new CBC_Mode<Camellia>::Encryption);
    }
    else if(algorithm == "DES-EDE3-CBC")
    {
        ksize = 24;
        vsize = 8;

        stream.reset(new CBC_Mode<DES_EDE3>::Encryption);
    }
    else if(algorithm == "IDEA-CBC")
    {
        ksize = 16;
        vsize = 8;

        stream.reset(new CBC_Mode<IDEA>::Encryption);
    }
    else if(algorithm == "DES-CBC")
    {
        ksize = 8;
        vsize = 8;

        stream.reset(new CBC_Mode<DES>::Encryption);
    }
    else
    {
        throw NotImplemented("PEM_CipherForAlgorithm: '" + algorithm + "' is not implemented");
    }

    const unsigned char* _pword = reinterpret_cast<const unsigned char*>(password);
    const size_t _plen = length;

    SecByteBlock _key(ksize), _iv(vsize), _salt(vsize);

    // The IV pulls double duty. First, the first PKCS5_SALT_LEN bytes are used
    //   as the Salt in EVP_BytesToKey. Second, its used as the IV in the cipher.
    // assert(_iv.size() >= OPENSSL_PKCS5_SALT_LEN);

    rng.GenerateBlock(_iv.data(), _iv.size());
    _salt = _iv;

    // MD5 is engrained OpenSSL goodness. MD5, IV and Password are IN; KEY is OUT.
    //   {NULL,0} parameters are the OUT IV. However, the original IV in the PEM
    //   header is used; and not the derived IV.
    Weak::MD5 md5;
    int ret = OPENSSL_EVP_BytesToKey(md5, _salt.data(), _pword, _plen, 1, _key.data(), _key.size(), NULL, 0);
    if(ret != static_cast<int>(ksize))
        throw Exception(Exception::OTHER_ERROR, "PEM_CipherForAlgorithm: EVP_BytesToKey failed");

    SymmetricCipher* cipher = dynamic_cast<SymmetricCipher*>(stream.get());
    // assert(cipher != NULL);

    cipher->SetKeyWithIV(_key.data(), _key.size(), _iv.data(), _iv.size());

    _key.swap(key);
    _iv.swap(iv);
}

void PEM_Encrypt(BufferedTransformation& src, BufferedTransformation& dest, member_ptr<StreamTransformation>& stream)
{
    StreamTransformationFilter filter(*stream, new Redirector(dest));
    src.TransferTo(filter);
    filter.MessageEnd();
}

void PEM_EncryptAndEncode(BufferedTransformation& src, BufferedTransformation& dest, member_ptr<StreamTransformation>& stream)
{
    ByteQueue temp;
    PEM_Encrypt(src, temp, stream);

    PEM_Base64Encode(temp, dest);
}

template <class EC>
bool PEM_GetNamedCurve(const DL_GroupParameters_EC<EC>& params)
{
    return params.GetEncodeAsOID();
}

template <class EC>
void PEM_SetNamedCurve(const DL_GroupParameters_EC<EC>& params, bool flag)
{
    DL_GroupParameters_EC<EC>& pp = const_cast<DL_GroupParameters_EC<EC>&>(params);
    pp.SetEncodeAsOID(flag);
}

ANONYMOUS_NAMESPACE_END

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

NAMESPACE_BEGIN(CryptoPP)

void PEM_Save(BufferedTransformation& bt, const RSA::PublicKey& rsa)
{
    PEM_SavePublicKey(bt, rsa, PUBLIC_BEGIN, PUBLIC_END);
}

void PEM_Save(BufferedTransformation& bt, const RSA::PrivateKey& rsa)
{
    PEM_SavePrivateKey(bt, rsa, RSA_PRIVATE_BEGIN, RSA_PRIVATE_END);
}

void PEM_Save(BufferedTransformation& bt, RandomNumberGenerator& rng, const RSA::PrivateKey& rsa, const string& algorithm, const char* password, size_t length)
{
    PEM_SavePrivateKey(bt, rng, rsa, RSA_PRIVATE_BEGIN, RSA_PRIVATE_END, algorithm, password, length);
}

void PEM_Save(BufferedTransformation& bt, const DSA::PublicKey& dsa)
{
    PEM_SavePublicKey(bt, dsa, PUBLIC_BEGIN, PUBLIC_END);
}

void PEM_Save(BufferedTransformation& bt, const DSA::PrivateKey& dsa)
{
    PEM_SavePrivateKey(bt, dsa, DSA_PRIVATE_BEGIN, DSA_PRIVATE_END);
}

void PEM_Save(BufferedTransformation& bt, RandomNumberGenerator& rng, const DSA::PrivateKey& dsa, const string& algorithm, const char* password, size_t length)
{
    PEM_SavePrivateKey(bt, rng, dsa, DSA_PRIVATE_BEGIN, DSA_PRIVATE_END, algorithm, password, length);
}

void PEM_Save(BufferedTransformation& bt, const DL_GroupParameters_EC<ECP>& params)
{
    bool old = PEM_GetNamedCurve(params);
    PEM_SetNamedCurve(params, true);

    PEM_SaveParams(bt, params, EC_PARAMETERS_BEGIN, EC_PARAMETERS_END);
    PEM_SetNamedCurve(params, old);
}

void PEM_Save(BufferedTransformation& bt, const DL_GroupParameters_EC<EC2N>& params)
{
    bool old = PEM_GetNamedCurve(params);
    PEM_SetNamedCurve(params, true);

    PEM_SaveParams(bt, params, EC_PARAMETERS_BEGIN, EC_PARAMETERS_END);
    PEM_SetNamedCurve(params, old);
}

void PEM_Save(BufferedTransformation& bt, const DL_PublicKey_EC<ECP>& ec)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePublicKey(bt, ec, PUBLIC_BEGIN, PUBLIC_END);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, const DL_PrivateKey_EC<ECP>& ec)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePrivateKey(bt, ec, EC_PRIVATE_BEGIN, EC_PRIVATE_END);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, RandomNumberGenerator& rng, const DL_PrivateKey_EC<ECP>& ec, const std::string& algorithm, const char* password, size_t length)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePrivateKey(bt, rng, ec, EC_PRIVATE_BEGIN, EC_PRIVATE_END, algorithm, password, length);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, const DL_PublicKey_EC<EC2N>& ec)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePublicKey(bt, ec, PUBLIC_BEGIN, PUBLIC_END);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, const DL_PrivateKey_EC<EC2N>& ec)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePrivateKey(bt, ec, EC_PRIVATE_BEGIN, EC_PRIVATE_END);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, RandomNumberGenerator& rng, const DL_PrivateKey_EC<EC2N>& ec, const std::string& algorithm, const char* password, size_t length)
{
    bool old = PEM_GetNamedCurve(ec.GetGroupParameters());
    PEM_SetNamedCurve(ec.GetGroupParameters(), true);

    PEM_SavePrivateKey(bt, rng, ec, EC_PRIVATE_BEGIN, EC_PRIVATE_END, algorithm, password, length);
    PEM_SetNamedCurve(ec.GetGroupParameters(), old);
}

void PEM_Save(BufferedTransformation& bt, const DL_Keys_ECDSA<ECP>::PrivateKey& ecdsa)
{
    PEM_Save(bt, dynamic_cast<const DL_PrivateKey_EC<ECP>&>(ecdsa));
}

void PEM_Save(BufferedTransformation& bt, RandomNumberGenerator& rng, DL_Keys_ECDSA<ECP>::PrivateKey& ecdsa, const std::string& algorithm, const char* password, size_t length)
{
    PEM_Save(bt, rng, dynamic_cast<DL_PrivateKey_EC<ECP>&>(ecdsa), algorithm, password, length);
}

void PEM_Save(BufferedTransformation& bt, const DL_GroupParameters_DSA& params)
{
    ByteQueue queue;

    PEM_WriteLine(queue, DSA_PARAMETERS_BEGIN);

    Base64Encoder encoder(new Redirector(queue), true /*lineBreak*/, RFC1421_LINE_BREAK);
    params.Save(encoder);
    encoder.MessageEnd();

    PEM_WriteLine(queue, DSA_PARAMETERS_END);

    queue.TransferTo(bt);
    bt.MessageEnd();
}

void PEM_DH_Save(BufferedTransformation& bt, const Integer& p, const Integer& g)
{
    ByteQueue queue;

    PEM_WriteLine(queue, DH_PARAMETERS_BEGIN);

    Base64Encoder encoder(new Redirector(queue), true /*lineBreak*/, RFC1421_LINE_BREAK);

    DERSequenceEncoder seq(encoder);
        p.BEREncode(seq);
        g.BEREncode(seq);
    seq.MessageEnd();

    encoder.MessageEnd();

    PEM_WriteLine(queue, DH_PARAMETERS_END);

    queue.TransferTo(bt);
    bt.MessageEnd();
}

void PEM_DH_Save(BufferedTransformation& bt, const Integer& p, const Integer& q, const Integer& g)
{
    ByteQueue queue;

    PEM_WriteLine(queue, DH_PARAMETERS_BEGIN);

    Base64Encoder encoder(new Redirector(queue), true /*lineBreak*/, RFC1421_LINE_BREAK);

    DERSequenceEncoder seq(encoder);
        p.BEREncode(seq);
        q.BEREncode(seq);
        g.BEREncode(seq);
    seq.MessageEnd();

    encoder.MessageEnd();

    PEM_WriteLine(queue, DH_PARAMETERS_END);

    queue.TransferTo(bt);
    bt.MessageEnd();
}

NAMESPACE_END