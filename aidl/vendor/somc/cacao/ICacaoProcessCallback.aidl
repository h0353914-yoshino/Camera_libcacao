package vendor.somc.cacao;

import vendor.somc.cacao.CacaoBlob;

@VintfStability
oneway interface ICacaoProcessCallback {
    void notifyProgress(in CacaoBlob data);
    void notifyResult(in CacaoBlob data);
}
