package com.jlink.service;

import com.sun.net.httpserver.*;
import java.io.*;
import java.net.*;
import java.util.*;
import java.util.concurrent.Executors;

/**
 * JLinkServer —— 连接 Creo 的 J-Link HTTP/REST 服务（零外部依赖，基于 JDK 内置 HttpServer）。
 *
 * 启动: java com.jlink.service.JLinkServer [port]
 * 所有涉及 Creo 的调用都在 CreoLink.lock 内串行执行（J-Link 单线程约束）。
 * 具体端点逻辑见 Handlers 类。
 */
public class JLinkServer {

    static int port = 8080;
    static HttpServer server;

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            try { port = Integer.parseInt(args[0]); } catch (Exception ignore) {}
        }
        CreoLink.configure(env("CREO_HOME", "C:\\Program Files\\PTC\\Creo 9.0.8.0"),
                           env("LOG_DIR", "logs"));

        server = HttpServer.create(new InetSocketAddress(port), 0);
        server.setExecutor(Executors.newFixedThreadPool(4));

        ctx("/",               Handlers::index);
        ctx("/health",         Handlers::health);
        ctx("/connect",        Handlers::connect);
        ctx("/start",          Handlers::start);
        ctx("/disconnect",     Handlers::disconnect);
        ctx("/session",        Handlers::session);
        ctx("/models",         Handlers::models);
        ctx("/model/current",  Handlers::currentModel);
        ctx("/model/open",     Handlers::openModel);
        ctx("/model/save",     Handlers::saveModel);
        ctx("/model/erase",    Handlers::eraseModel);
        ctx("/model/display",  Handlers::displayModel);
        ctx("/params",         Handlers::params);
        ctx("/param",          Handlers::param);
        ctx("/material",       Handlers::material);
        ctx("/dir",            Handlers::dir);
        ctx("/image",          Handlers::image);
        ctx("/export",         Handlers::exportModel);
        ctx("/model/rename",   Handlers::renameModel);
        ctx("/bom",            Handlers::bom);
        ctx("/bomtree",        Handlers::bomTree);

        server.start();
        banner();

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            System.out.println("正在关闭服务并断开 Creo...");
            try { CreoLink.disconnect(); } catch (Exception ignore) {}
            if (server != null) server.stop(1);
        }));
    }

    static void ctx(String path, HttpHandler h) {
        server.createContext(path, exchange -> {
            Headers hh = exchange.getResponseHeaders();
            hh.add("Access-Control-Allow-Origin", "*");
            hh.add("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
            hh.add("Access-Control-Allow-Headers", "Content-Type");
            if ("OPTIONS".equalsIgnoreCase(exchange.getRequestMethod())) {
                exchange.sendResponseHeaders(204, -1); exchange.close(); return;
            }
            try { h.handle(exchange); }
            catch (Throwable t) {
                Handlers.sendJson(exchange, 500,
                    MiniJson.obj("ok", false, "error", String.valueOf(t)));
            }
        });
    }

    static String env(String k, String def) {
        String v = System.getenv(k);
        return (v != null && !v.isEmpty()) ? v : def;
    }

    static void banner() {
        System.out.println("======================================================");
        System.out.println("  J-Link -> Creo HTTP 服务已启动");
        System.out.println("  监听端口 : http://localhost:" + port);
        System.out.println("  Creo 主目录: " + CreoLink.creoHome);
        System.out.println("  后台无图形模式: /start 自动拉起 nmsd + xtop 并连接");
        System.out.println("  首页/文档: http://localhost:" + port + "/");
        System.out.println("======================================================");
    }
}
