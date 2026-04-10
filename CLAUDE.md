# CLAUDE.md

## Visão Geral do Projeto

Este projeto implementa uma nova action para o `ipfw` no FreeBSD, com o objetivo de adicionar o endereço de origem ou destino de um pacote em uma **table** do `ipfw`, caso o pacote dê match em uma regra.

**Versão alvo:** FreeBSD 15/stable (primeira versão)

---

## O que é e como funciona

O `ipfw` é o firewall nativo do FreeBSD. Ele suporta **tables** — estruturas de dados que armazenam conjuntos de endereços IP, redes, portas ou outros valores, usadas para correspondência dinâmica em regras.

A nova action `addtable` (nome sujeito a definição final) permite que, ao invés de apenas aceitar ou bloquear um pacote, uma regra possa **inserir dinamicamente** o endereço IP de origem (`src`) ou destino (`dst`) do pacote em uma table especificada.

Exemplo de uso pretendido:

```
ipfw add 100 addtable 10 src from any to any
ipfw add 200 addtable 10 dst from any to 192.168.1.1
```

---

## Estrutura do Projeto

O código será desenvolvido como uma modificação/extensão do kernel do FreeBSD e das ferramentas de userspace do `ipfw`.

### Componentes principais esperados

| Componente | Localização (árvore FreeBSD) | Descrição |
|---|---|---|
| Kernel - lógica de regras | `sys/netpfil/ipfw/ip_fw_rules.c` | Processamento de regras e actions |
| Kernel - tables | `sys/netpfil/ipfw/ip_fw_table.c` | Manipulação de tables no kernel |
| Userspace - ipfw CLI | `sbin/ipfw/ipfw2.c` | Parser de comandos e montagem de regras |
| Userspace - tables CLI | `sbin/ipfw/tables.c` | Comandos relacionados a tables |

---

## Ambiente de Desenvolvimento

- **Sistema operacional:** FreeBSD 15/stable
- **Compilador:** clang (padrão do FreeBSD base)
- **Build system:** `make` com o sistema de build do FreeBSD (`src.conf`, `make.conf`)

### Configuração do ambiente

1. Obter o código-fonte do FreeBSD 15/stable:
   ```sh
   git clone --branch stable/15 https://git.freebsd.org/src.git /usr/src
   ```

2. Para compilar apenas o kernel:
   ```sh
   cd /usr/src
   make -j$(sysctl -n hw.ncpu) buildkernel KERNCONF=GENERIC
   ```

3. Para compilar apenas o userspace do `ipfw`:
   ```sh
   cd /usr/src/sbin/ipfw
   make
   ```

---

## Convenções de Código

- Seguir o estilo de código do FreeBSD: [FreeBSD style(9)](https://man.freebsd.org/cgi/man.cgi?style)
- Indentação com **tabs** de 8 espaços
- Nomes de funções e variáveis em `snake_case`
- Comentários em inglês (padrão da base do FreeBSD)
- Novos opcodes de action devem ser adicionados em `ip_fw.h` com prefixo `O_`

---

## Decisões de Design (requisitos obrigatórios)

### 1. Ação não-terminal — continua o processamento

`addtable` é uma **ação não-terminal**: após enfileirar a adição do endereço na tabela, o processamento do pacote **continua** normalmente para as regras seguintes. O comportamento é equivalente ao de `count` — o pacote não é aceito nem descartado pela action, apenas o endereço é registrado na tabela.

Isso implica que:
- No kernel (`ip_fw2.c`), o case `O_ADDTABLE` deve usar o padrão `l = 0; break` das ações não-terminais como `O_COUNT`, sem definir `retval` nem `done`.
- O opcode deve estar na lista `action_opcodes[]` no userspace (`ipfw2.c`) apenas para fins de exibição (`ipfw show`).

### 2. Validação de existência da table no momento da criação da regra

Ao adicionar uma regra com `addtable <tblno>`, o kernel (ou o parser userspace) deve verificar se a table `<tblno>` **já existe**. Se a table não existir, o comando deve falhar com erro (ex: `ESRCH` / "Table not found") e a regra **não deve ser criada**.

Isso garante que:
- Não haja regras "orphan" referenciando tables inexistentes.
- O comportamento é consistente com outros opcodes que referenciam objetos nomeados do ipfw (tables de lookup, NAT instances, etc.), os quais também exigem que o objeto exista antes da criação da regra.

**Implementação sugerida:** validar no handler do `TOK_ADDTABLE` no userspace (via `getsockopt`/`setsockopt` para checar existência da table antes de submeter a regra), ou no `ipfw_check_opcode()` do kernel durante a validação de `O_ADDTABLE`.

---

## Checklist de Testes

Executar sempre que o usuário pedir explicitamente para testar. Usar o
binário compilado em `/usr/src/sbin/ipfw/ipfw` e o módulo em
`/usr/obj/usr/src/amd64.amd64/sys/modules/ipfw/ipfw.ko`.

```sh
IPFW=/usr/src/sbin/ipfw/ipfw
KO=/usr/obj/usr/src/amd64.amd64/sys/modules/ipfw/ipfw.ko

# 1. Carregar o módulo
kldload $KO && kldstat | grep ipfw

# 2. Rejeição com table inexistente (deve falhar com ESRCH / "No such process")
$IPFW add 9000 addtable 99 src ip from any to any 2>&1

# 3. Criar tables e adicionar rules
$IPFW table 99 create type addr
$IPFW table 98 create type addr
$IPFW add 9000 addtable 99 src ip  from any to any   # src IPv4
$IPFW add 9001 addtable 99 dst ip  from any to any   # dst IPv4
$IPFW add 9002 addtable 98 src ip6 from any to any   # src IPv6
$IPFW add 9003 addtable 98 dst ip6 from any to any   # dst IPv6

# 4. Exibição correta (ipfw show)
$IPFW show 9000 9001 9002 9003

# 5. Inserção de src IPv4
ping -c 2 -q 127.0.0.1 > /dev/null && sleep 0.3
$IPFW table 99 list   # deve conter 127.0.0.1/32

# 6. Inserção de dst IPv4
ping -c 2 -q 127.0.0.2 > /dev/null && sleep 0.3
$IPFW table 99 list   # deve conter 127.0.0.2/32

# 7. Inserção de src e dst IPv6
ping6 -c 2 -q ::1 > /dev/null && sleep 0.3
$IPFW table 98 list   # deve conter ::1/128

# 8. Ação não-terminal: regra subsequente ainda é avaliada
$IPFW add 9004 deny ip from 127.0.0.3 to any
ping -c 1 -q 127.0.0.3 > /dev/null 2>&1
echo "ping exit: $? (esperado != 0 — bloqueado pela rule 9004)"
sleep 0.3
$IPFW table 99 list | grep 127.0.0.3   # src deve ter sido inserido antes do deny

# Cleanup
$IPFW delete 9000 9001 9002 9003 9004
$IPFW table 99 destroy
$IPFW table 98 destroy
kldunload ipfw
```

---

## Referências

- [ipfw(8) man page](https://man.freebsd.org/cgi/man.cgi?ipfw)
- [FreeBSD Handbook - Firewalls](https://docs.freebsd.org/en/books/handbook/firewalls/)
- Código-fonte do ipfw: `sys/netpfil/ipfw/` e `sbin/ipfw/` na árvore do FreeBSD
