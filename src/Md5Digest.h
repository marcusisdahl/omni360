#ifndef MD5_DIGEST_H
#define MD5_DIGEST_H

// MD5 (RFC 1321) - what par2 identifies files, slices and its own packets by.
// Not for anything security-related. Free of any Xbox header, so it's tested
// on a PC (tools/test_usenet).

struct Md5
{
    unsigned long state[4];
    unsigned long long length; // bytes so far
    unsigned char block[64];
    unsigned long used;        // bytes of block filled
};

void Md5Init(Md5 *md5);
void Md5Update(Md5 *md5, const void *data, unsigned long len);
void Md5Final(Md5 *md5, unsigned char digest[16]);

// The digest of len bytes, in one go.
void Md5Of(const void *data, unsigned long len, unsigned char digest[16]);

#endif
