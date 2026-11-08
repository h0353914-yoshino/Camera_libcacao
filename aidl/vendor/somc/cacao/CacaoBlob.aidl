package vendor.somc.cacao;

// 取代 SerializedData(fd 清單) + IMemory：payload 走共享記憶體 fd，size 為有效長度，
// fds 為 SerializedData 內帶的額外 fd（最多 95 個）。
@VintfStability
parcelable CacaoBlob {
    @nullable ParcelFileDescriptor mem;
    int size;
    ParcelFileDescriptor[] fds;
}
