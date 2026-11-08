package vendor.somc.cacao;

import vendor.somc.cacao.CacaoBlob;

@VintfStability
oneway interface ICacaoClient {
    void notifyEvent(int eventType, in CacaoBlob data);
}
